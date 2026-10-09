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
    CHECK(aborted->state == "running");
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
