#include "agent/harness/TaskScheduler.hpp"
#include "agent/harness/TranscriptExport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>

using namespace cch;
using namespace std::chrono_literals;

TEST_CASE("TaskScheduler requires resume and commits running before handler execution", "[agent][task][durable]") {
    auto store = agent::session::TaskStore::open_memory();
    REQUIRE(store);
    const agent::session::DurableTask task{
            .id = "deferred", .kind = "work", .state = "pending", .checkpoint = "", .owner_session = ""};
    REQUIRE(store->create_task(task));
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    harness::TaskScheduler scheduler(*store);
    REQUIRE(scheduler.enqueue(task, [&](std::stop_token) -> support::Expected<std::string> {
        auto loaded = store->load_task("deferred");
        REQUIRE(loaded);
        REQUIRE(loaded->state == "running");
        {
            std::lock_guard lock(mutex);
            started = true;
        }
        changed.notify_all();
        return "done";
    }));
    std::this_thread::sleep_for(20ms);
    CHECK_FALSE(started);
    scheduler.resume();
    {
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, 2s, [&] { return started; }));
    }
    scheduler.close();
    auto completed = store->load_task("deferred");
    REQUIRE(completed);
    CHECK(completed->state == "completed");
    CHECK(completed->checkpoint == "done");
}

TEST_CASE("TaskScheduler recovery dispatches registered work only after resume", "[agent][task][durable]") {
    auto store = agent::session::TaskStore::open_memory();
    REQUIRE(store);
    const agent::session::DurableTask task{
            .id = "recovered", .kind = "work", .state = "running", .checkpoint = "checkpoint", .owner_session = ""};
    REQUIRE(store->create_task(task));
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    harness::TaskScheduler scheduler(*store);
    REQUIRE(scheduler.register_handler(task, [&](std::stop_token) -> support::Expected<std::string> {
        {
            std::lock_guard lock(mutex);
            started = true;
        }
        changed.notify_all();
        return "recovered result";
    }));
    std::this_thread::sleep_for(20ms);
    CHECK_FALSE(started);
    scheduler.resume();
    {
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, 2s, [&] { return started; }));
    }
    scheduler.close();
    auto loaded = store->load_task("recovered");
    REQUIRE(loaded);
    CHECK(loaded->state == "completed");
}

TEST_CASE("TaskScheduler does not run a handler when reservation storage fails", "[agent][task][durable]") {
    auto db = agent::session::SqliteDatabase::open_memory();
    REQUIRE(db);
    agent::session::TaskStore store(std::move(*db));
    REQUIRE(store.init_schema());
    const agent::session::DurableTask task{.id = "reservation-failure",
            .kind = "work",
            .state = "pending",
            .checkpoint = "before",
            .owner_session = ""};
    REQUIRE(store.create_task(task));
    REQUIRE(store.db_for_test().execute("CREATE TRIGGER reject_reservation BEFORE UPDATE ON tasks BEGIN SELECT "
                                        "RAISE(ABORT, 'injected reservation failure'); END;"));
    bool started = false;
    harness::TaskScheduler scheduler(store);
    REQUIRE(scheduler.enqueue(task, [&](std::stop_token) -> support::Expected<std::string> {
        started = true;
        return "unexpected";
    }));
    scheduler.resume();
    scheduler.close();
    CHECK_FALSE(started);
    auto loaded = store.load_task(task.id);
    REQUIRE(loaded);
    CHECK(loaded->state == "pending");
    CHECK(loaded->checkpoint == "before");
}

TEST_CASE("TaskScheduler recovers transcript export and makes the file effect idempotent",
        "[agent][task][durable][issue928]") {
    const auto database = std::filesystem::temp_directory_path() / "cch-task-export-928.sqlite";
    const auto destination = std::filesystem::temp_directory_path() / "cch-task-export-928.jsonl";
    std::filesystem::remove(database);
    std::filesystem::remove(destination);
    auto transcript = harness::session::SessionStore::in_memory();
    {
        auto store = agent::session::TaskStore::open(database);
        REQUIRE(store);
        const agent::session::DurableTask task{.id = "export-928",
                .kind = "transcript_export",
                .state = "pending",
                .checkpoint = "",
                .owner_session = "session"};
        auto submission = store->submit(task, "request-export-928", "transcript_export", "{}");
        REQUIRE(submission);
        REQUIRE(store->transition(task.id, "pending", "running", ""));
        const auto effect = harness::transcript_export_handler(transcript, destination.string())({});
        REQUIRE(effect);
        auto durable = store->load_task(task.id);
        REQUIRE(durable);
        CHECK(durable->state == "running");
    }
    std::ifstream input(destination);
    const std::string first((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    {
        auto store = agent::session::TaskStore::open(database);
        REQUIRE(store);
        harness::TaskScheduler scheduler(*store);
        const agent::session::DurableTask recovered{.id = "export-928",
                .kind = "transcript_export",
                .state = "running",
                .checkpoint = "",
                .owner_session = "session"};
        REQUIRE(scheduler.register_handler(
                recovered, harness::transcript_export_handler(transcript, destination.string())));
        scheduler.resume();
        bool completed = false;
        for (int attempt = 0; attempt < 2000; ++attempt) {
            auto durable = store->load_task("export-928");
            REQUIRE(durable);
            if (durable->state == "completed") {
                completed = true;
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
        CHECK(completed);
        scheduler.close();
        auto resolved = store->load_submission("request-export-928");
        REQUIRE(resolved);
        CHECK(resolved->state == "done");
    }
    std::ifstream replayed_input(destination);
    const std::string replayed((std::istreambuf_iterator<char>(replayed_input)), std::istreambuf_iterator<char>());
    CHECK(replayed == first);
    std::filesystem::remove(database);
    std::filesystem::remove(destination);
}

TEST_CASE("TaskScheduler cascades abort to active and queued descendants", "[agent][task][durable][issue933]") {
    auto store = agent::session::TaskStore::open_memory();
    REQUIRE(store);
    const agent::session::DurableTask parent{.id = "parent-933", .kind = "work", .state = "pending"};
    const agent::session::DurableTask child{
            .id = "child-933", .kind = "work", .state = "pending", .parent_task_id = parent.id};
    const agent::session::DurableTask grandchild{
            .id = "grandchild-933", .kind = "work", .state = "pending", .parent_task_id = child.id};
    REQUIRE(store->create_task(parent));
    REQUIRE(store->create_task(child));
    REQUIRE(store->create_task(grandchild));
    std::mutex mutex;
    std::condition_variable changed;
    bool child_started = false;
    bool child_stopped = false;
    std::atomic_bool grandchild_started{false};
    harness::TaskScheduler scheduler(*store);
    REQUIRE(scheduler.enqueue(child, [&](std::stop_token token) -> support::Expected<std::string> {
        {
            std::lock_guard lock(mutex);
            child_started = true;
        }
        changed.notify_all();
        while (!token.stop_requested())
            std::this_thread::sleep_for(1ms);
        child_stopped = true;
        return "cancelled";
    }));
    REQUIRE(scheduler.enqueue(grandchild, [&](std::stop_token) -> support::Expected<std::string> {
        grandchild_started.store(true);
        return "unexpected";
    }));
    scheduler.resume();
    {
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, 2s, [&] { return child_started; }));
    }
    REQUIRE(scheduler.request_abort(parent.id));
    scheduler.close();
    CHECK(child_stopped);
    CHECK_FALSE(grandchild_started.load());
    for (const auto& id : {parent.id, child.id, grandchild.id}) {
        auto task = store->load_task(id);
        REQUIRE(task);
        CHECK(task->state == "aborted");
    }
}

TEST_CASE("TaskScheduler propagates durable abort and close waits for terminalization", "[agent][task][durable]") {
    auto store = agent::session::TaskStore::open_memory();
    REQUIRE(store);
    const agent::session::DurableTask task{
            .id = "abortable", .kind = "work", .state = "pending", .checkpoint = "", .owner_session = ""};
    REQUIRE(store->submit(task, "abort-request", "work", "{}"));
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    bool stopped = false;
    harness::TaskScheduler scheduler(*store);
    REQUIRE(scheduler.enqueue(task, [&](std::stop_token token) -> support::Expected<std::string> {
        {
            std::lock_guard lock(mutex);
            started = true;
        }
        changed.notify_all();
        while (!token.stop_requested())
            std::this_thread::sleep_for(1ms);
        stopped = true;
        return "cancelled";
    }));
    scheduler.resume();
    {
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, 2s, [&] { return started; }));
    }
    REQUIRE(scheduler.request_abort("abortable"));
    scheduler.close();
    CHECK(stopped);
    auto loaded = store->load_task("abortable");
    REQUIRE(loaded);
    CHECK(loaded->abort_requested);
    CHECK(loaded->state == "aborted");
    auto submission = store->load_submission("abort-request");
    REQUIRE(submission);
    CHECK(submission->state == "unanswered");
}
