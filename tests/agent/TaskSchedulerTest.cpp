#include "agent/harness/TaskScheduler.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
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

TEST_CASE("TaskScheduler propagates durable abort and close waits for terminalization", "[agent][task][durable]") {
    auto store = agent::session::TaskStore::open_memory();
    REQUIRE(store);
    const agent::session::DurableTask task{
            .id = "abortable", .kind = "work", .state = "pending", .checkpoint = "", .owner_session = ""};
    REQUIRE(store->create_task(task));
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
}
