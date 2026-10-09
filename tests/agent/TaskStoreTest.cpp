#include "agent/harness/session/SqliteDatabase.hpp"
#include "agent/harness/session/TaskStore.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace cch;

TEST_CASE("TaskStore persists transitions, checkpoints, abort marks, and recovery", "[agent][durable][task]") {
    auto store = agent::session::TaskStore::open_memory();
    REQUIRE(store);
    const agent::session::DurableTask task{.id = "task-1",
            .kind = "build",
            .state = "pending",
            .checkpoint = "{}",
            .owner_session = "session-1",
            .definition_version = 1};
    REQUIRE(store->create_task(task));
    REQUIRE(store->add_submission(
            {.id = "submission-1", .task_id = task.id, .kind = "build", .payload = "{}", .state = "queued"}));
    REQUIRE(store->transition(task.id, "pending", "running", "{\"step\":1}"));
    REQUIRE(store->transition(task.id, "running", "completing", "{\"step\":2}"));
    REQUIRE(store->transition(task.id, "completing", "completed", "{\"step\":3}"));
    CHECK_FALSE(store->transition(task.id, "completed", "running", "{}"));

    REQUIRE(store->create_task(
            {.id = "task-2", .kind = "build", .state = "running", .checkpoint = "{}", .owner_session = ""}));
    REQUIRE(store->create_task(
            {.id = "task-3", .kind = "build", .state = "running", .checkpoint = "{}", .owner_session = ""}));
    REQUIRE(store->request_abort("task-3"));
    auto abort_transition = store->transition("task-3", "running", "completing", "ignored");
    CHECK_FALSE(abort_transition);
    auto recovered = store->recover_tasks();
    REQUIRE(recovered);
    REQUIRE(recovered->size() == 1);
    CHECK(recovered->front().id == "task-2");
    CHECK(recovered->front().state == "pending");
    auto completed = store->load_task("task-1");
    REQUIRE(completed);
    CHECK(completed->checkpoint == "{\"step\":3}");
    auto aborted = store->load_task("task-3");
    REQUIRE(aborted);
    CHECK(aborted->state == "aborted");
    CHECK(aborted->abort_requested);
}

TEST_CASE("TaskStore deduplicates submissions across reopen", "[agent][durable][task][issue928]") {
    const auto path = std::filesystem::temp_directory_path() / "cch-task-submission-928.sqlite";
    std::filesystem::remove(path);
    {
        auto store = agent::session::TaskStore::open(path);
        REQUIRE(store);
        const agent::session::DurableTask task{.id = "task-request-928",
                .kind = "transcript_export",
                .state = "pending",
                .checkpoint = "",
                .owner_session = "session"};
        auto first = store->submit(task, "request-928", "transcript_export", "{\\\"path\\\":\\\"transcript.jsonl\\\"}");
        REQUIRE(first);
        CHECK(first->state == "queued");
    }
    {
        auto store = agent::session::TaskStore::open(path);
        REQUIRE(store);
        const agent::session::DurableTask duplicate{.id = "different-task-id",
                .kind = "other",
                .state = "pending",
                .checkpoint = "",
                .owner_session = "session"};
        auto second = store->submit(duplicate, "request-928", "other", "{}");
        REQUIRE(second);
        CHECK(second->task_id == "task-request-928");
        CHECK(second->kind == "transcript_export");
    }
    std::filesystem::remove(path);
}

TEST_CASE("TaskStore aborts descendants and fails joined children with their parent",
        "[agent][durable][task][issue933]") {
    auto store = agent::session::TaskStore::open_memory();
    REQUIRE(store);
    REQUIRE(store->create_task({.id = "parent", .kind = "work", .state = "pending"}));
    REQUIRE(store->create_task({.id = "child", .kind = "work", .state = "pending", .parent_task_id = "parent"}));
    REQUIRE(store->create_task({.id = "grandchild", .kind = "work", .state = "pending", .parent_task_id = "child"}));
    REQUIRE(store->request_abort("parent"));
    for (const auto& id : {"parent", "child", "grandchild"}) {
        auto task = store->load_task(id);
        REQUIRE(task);
        CHECK(task->abort_requested);
        CHECK(task->state == "aborted");
    }

    REQUIRE(store->create_task({.id = "failed-parent", .kind = "work", .state = "running"}));
    REQUIRE(store->create_task(
            {.id = "joined-child", .kind = "work", .state = "running", .parent_task_id = "failed-parent"}));
    REQUIRE(store->transition("failed-parent", "running", "completing", ""));
    REQUIRE(store->transition("failed-parent", "completing", "failed", ""));
    auto child = store->load_task("joined-child");
    REQUIRE(child);
    CHECK(child->state == "failed");
}

TEST_CASE("TaskStore migrates task ownership and recovers parents first", "[agent][durable][task][issue933]") {
    auto db = agent::session::SqliteDatabase::open_memory();
    REQUIRE(db);
    REQUIRE(db->execute(
            "CREATE TABLE tasks (id TEXT PRIMARY KEY, kind TEXT NOT NULL, state TEXT NOT NULL, "
            "checkpoint TEXT NOT NULL, owner_session TEXT NOT NULL, definition_version INTEGER NOT NULL, "
            "created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL, abort_requested INTEGER NOT NULL DEFAULT 0);"));
    REQUIRE(db->execute("INSERT INTO tasks VALUES ('child','work','running','','',0,1,1,0), "
                        "('parent','work','running','','',0,1,1,0);"));
    agent::session::TaskStore store(std::move(*db));
    REQUIRE(store.init_schema());
    auto child = store.load_task("child");
    REQUIRE(child);
    CHECK(child->parent_task_id.empty());
    REQUIRE(store.db_for_test().execute("UPDATE tasks SET parent_task_id = 'parent' WHERE id = 'child';"));
    REQUIRE(store.create_task({.id = "new-child", .kind = "work", .state = "running", .parent_task_id = "child"}));
    auto recovered = store.recover_tasks();
    REQUIRE(recovered);
    REQUIRE(recovered->size() == 3);
    CHECK((*recovered)[0].id == "parent");
    CHECK((*recovered)[1].id == "child");
    CHECK((*recovered)[2].id == "new-child");
}

TEST_CASE("TaskStore rolls back state and checkpoint on a failed transition", "[agent][durable][task]") {
    auto db = agent::session::SqliteDatabase::open_memory();
    REQUIRE(db);
    agent::session::TaskStore failing_store(std::move(*db));
    REQUIRE(failing_store.init_schema());
    REQUIRE(failing_store.create_task(
            {.id = "task-rollback", .kind = "build", .state = "pending", .checkpoint = "before", .owner_session = ""}));
    REQUIRE(failing_store.db_for_test().execute("CREATE TRIGGER reject_checkpoint BEFORE UPDATE ON tasks BEGIN SELECT "
                                                "RAISE(ABORT, 'injected failure'); END;"));
    CHECK_FALSE(failing_store.transition("task-rollback", "pending", "running", "after"));
    auto task = failing_store.load_task("task-rollback");
    REQUIRE(task);
    CHECK(task->state == "pending");
    CHECK(task->checkpoint == "before");
}
