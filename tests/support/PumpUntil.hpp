#pragma once

#include <boost/asio/io_context.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

namespace cch::tests {

/// Pump one loop until `done` becomes true (or the budget expires). Used
/// instead of `io.run()` because a live RuntimeRoot holds a work guard, so
/// `run()` would not return while the root is alive. Session persistence
/// outcomes also return from Runtime worker threads through the fixture
/// Runtime loop, so plain drain_ready can go idle while a prompt is still
/// settling off-thread. Service one ready handler per iteration: `poll()` can
/// never return when a loader or animation continually reposts render work,
/// which would make the timeout ineffective under scheduler pressure.
[[nodiscard]] inline bool pump_until(
    boost::asio::io_context& io,
    const std::atomic<bool>& done,
    std::chrono::milliseconds budget = std::chrono::milliseconds{10000}) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        if (io.stopped()) {
            io.restart();
        }
        (void)io.poll_one();
        std::this_thread::sleep_for(std::chrono::microseconds{100});
    }
    return done.load(std::memory_order_acquire);
}

/// Pump one loop until `done()` returns true (or the budget expires): the
/// predicate form lets the test poll a composed condition (for example a
/// loop-serviced flag together with a delivery count) without relying on a
/// one-shot handler that can never re-check. Ready work is serviced one
/// handler at a time so the deadline remains effective when another handler
/// continually reposts work.
[[nodiscard]] inline bool pump_until(
    boost::asio::io_context& io,
    const std::function<bool()>& done,
    std::chrono::milliseconds budget = std::chrono::milliseconds{10000}) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        if (io.stopped()) {
            io.restart();
        }
        (void)io.poll_one();
        std::this_thread::sleep_for(std::chrono::microseconds{100});
    }
    return done();
}

/// Join a co_spawned test run when the test scope ends: if the outcome has
/// not arrived, pump briefly so the completion handler runs while every
/// reference the run captures (terminal, session) is still alive. Declare it
/// after the run's captured locals so destruction pumps before they die.
/// Best-effort by design: in the strict no-exception shards a failed REQUIRE
/// terminates without unwinding, so only paths where destructors run reach
/// this guard; the short budget keeps a genuinely stuck failure path from
/// hanging the shard.
struct RunJoinGuard {
    RunJoinGuard(boost::asio::io_context& loop, std::function<bool()> predicate)
        : io(loop), done(std::move(predicate)) {}

    RunJoinGuard(const RunJoinGuard&) = delete;
    RunJoinGuard& operator=(const RunJoinGuard&) = delete;

    ~RunJoinGuard() {
        if (!done()) {
            (void)pump_until(io, done, std::chrono::milliseconds{1000});
        }
    }

    boost::asio::io_context& io;
    std::function<bool()> done;
};

} // namespace cch::tests
