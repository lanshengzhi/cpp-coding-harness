#include "support/PumpUntil.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

using namespace cch;

TEST_CASE(
    "drain_ready returns once the loop stays quiet",
    "[support][pump][issue553]") {
    boost::asio::io_context io;
    bool ran = false;
    boost::asio::post(io, [&] { ran = true; });

    const auto started = std::chrono::steady_clock::now();
    tests::drain_ready(io);

    CHECK(ran);
    // No producer is live, so the quiet window is the only wait.
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds{5});
}

TEST_CASE(
    "drain_ready returns within its backstop while slow work continually reposts",
    "[support][pump][issue553]") {
    boost::asio::io_context io;
    const auto guard = boost::asio::make_work_guard(io);
    std::atomic<bool> stop{false};
    // A live status animation keeps render work continuously ready, and under
    // sanitizer load one render pass can outlast the animation cadence: the
    // ready queue never observes an empty moment. Model that with a slow
    // handler that reposts itself (the exact #553 mechanism): the queue is
    // never observed empty, so an unbounded drain would never return and only
    // the backstop can end the loop.
    std::function<void()> repost;
    repost = [&] {
        if (stop.load(std::memory_order_acquire)) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        boost::asio::post(io, repost);
    };
    boost::asio::post(io, repost);

    const auto started = std::chrono::steady_clock::now();
    tests::drain_ready(io, std::chrono::milliseconds{10}, std::chrono::milliseconds{250});
    const auto elapsed = std::chrono::steady_clock::now() - started;

    stop.store(true, std::memory_order_release);
    // Run the one already-posted handler (it observes `stop` and does not
    // repost) so no queued handler outlives the captured locals.
    tests::drain_ready(io);

    CHECK(elapsed >= std::chrono::milliseconds{200});
    CHECK(elapsed < std::chrono::seconds{10});
}
