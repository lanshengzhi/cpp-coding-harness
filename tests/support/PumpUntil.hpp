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
/// settling off-thread. The budget only bounds the already-failing path: a
/// satisfied condition returns immediately.
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
        (void)io.poll();
        std::this_thread::sleep_for(std::chrono::microseconds{100});
    }
    return done.load(std::memory_order_acquire);
}

/// Pump one loop until `done()` returns true (or the budget expires): the
/// predicate form lets the test poll a composed condition (for example a
/// loop-serviced flag together with a delivery count) without relying on a
/// one-shot handler that can never re-check.
[[nodiscard]] inline bool pump_until(
    boost::asio::io_context& io,
    const std::function<bool()>& done,
    std::chrono::milliseconds budget = std::chrono::milliseconds{10000}) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        if (io.stopped()) {
            io.restart();
        }
        (void)io.poll();
        std::this_thread::sleep_for(std::chrono::microseconds{100});
    }
    return done();
}

} // namespace cch::tests
