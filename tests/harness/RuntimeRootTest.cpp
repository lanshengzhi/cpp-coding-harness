#include "harness/RuntimeRoot.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

using namespace cch;

namespace {

/// Pump one loop until `done` becomes true (or the budget expires). Used
/// instead of `io.run()` because a live RuntimeRoot holds a work guard, so
/// `run()` would not return while the root is alive.
[[nodiscard]] bool pump_until(
    boost::asio::io_context& io,
    const std::atomic<bool>& done,
    std::chrono::milliseconds budget = std::chrono::milliseconds{2000}) {
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

} // namespace

TEST_CASE(
    "RuntimeRoot rejects admission at operation-count saturation without running work",
    "[harness][runtime][issue459]") {
    auto io = std::make_shared<boost::asio::io_context>();
    harness::RuntimeRoot root(io, 1, 2, 1024 * 1024);
    auto target = root.make_target();

    std::atomic<std::size_t> worker_runs{0};
    auto* runs = &worker_runs;
    {
        auto first = target->try_admit(4);
        REQUIRE(first.has_value());
        REQUIRE(first->post_worker([runs]() noexcept { runs->fetch_add(1); }));
        auto second = target->try_admit(4);
        REQUIRE(second.has_value());
        REQUIRE(second->post_worker([runs]() noexcept { runs->fetch_add(1); }));

        // Capacity is exhausted: the third submission is rejected outright and
        // never creates a worker task (no inline fallback, no silent drop).
        CHECK_FALSE(target->try_admit(4).has_value());

        std::move(*first).complete([]() noexcept {});
        std::move(*second).complete([]() noexcept {});
    }
    while (io->poll() > 0) {
    }
    root.close();

    // The two admitted worker tasks ran; the rejected third never did.
    CHECK(worker_runs.load() == 2);
}

TEST_CASE(
    "RuntimeRoot rejects admission at byte-charge saturation",
    "[harness][runtime][issue459]") {
    auto io = std::make_shared<boost::asio::io_context>();
    harness::RuntimeRoot root(io, 1, 8, 100);
    auto target = root.make_target();

    auto first = target->try_admit(60);
    REQUIRE(first.has_value());
    // 60 + 60 exceeds the 100-byte budget.
    CHECK_FALSE(target->try_admit(60).has_value());
    // 60 + 40 fits exactly; the byte budget is charged, not just the count.
    auto small = target->try_admit(40);
    REQUIRE(small.has_value());
    CHECK_FALSE(target->try_admit(1).has_value());

    std::move(*first).complete([]() noexcept {});
    std::move(*small).complete([]() noexcept {});
    while (io->poll() > 0) {
    }
    root.close();
}

TEST_CASE(
    "input and timer callbacks progress while a gated worker holds an admission",
    "[harness][runtime][issue459]") {
    auto io = std::make_shared<boost::asio::io_context>();
    harness::RuntimeRoot root(io, 1, 4, 1024 * 1024);
    auto target = root.make_target();

    std::atomic<bool> worker_started{false};
    std::atomic<bool> release_worker{false};
    auto gate = target->try_admit(4);
    REQUIRE(gate.has_value());
    REQUIRE(gate->post_worker([&]() noexcept {
        worker_started.store(true, std::memory_order_release);
        while (!release_worker.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::microseconds{100});
        }
    }));
    // Wait until the worker occupies its admission and is gated.
    while (!worker_started.load(std::memory_order_acquire)) {
        if (io->stopped()) {
            io->restart();
        }
        (void)io->poll();
        std::this_thread::sleep_for(std::chrono::microseconds{100});
    }

    // The gated worker still holds its admission; a posted input callback and
    // a short timer must fire on the shared loop without waiting for it.
    std::atomic<bool> input_fired{false};
    std::atomic<bool> timer_fired{false};
    boost::asio::post(*io, [&]() noexcept { input_fired.store(true, std::memory_order_release); });
    boost::asio::steady_timer timer(*io, std::chrono::milliseconds{20});
    timer.async_wait([&](const boost::system::error_code&) {
        timer_fired.store(true, std::memory_order_release);
    });

    CHECK(pump_until(*io, input_fired));
    CHECK(pump_until(*io, timer_fired));

    release_worker.store(true, std::memory_order_release);
    root.close();
}

TEST_CASE(
    "RuntimeTarget delivers terminal results in admission order",
    "[harness][runtime][issue459]") {
    auto io = std::make_shared<boost::asio::io_context>();
    harness::RuntimeRoot root(io, 2, 8, 1024 * 1024);
    auto target = root.make_target();

    std::vector<int> delivered;
    {
        auto first = target->try_admit(4);
        auto second = target->try_admit(4);
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        // The second admission completes first; terminal delivery must still
        // follow the per-target admission sequence (1 then 2).
        std::move(*second).complete([&delivered]() noexcept { delivered.push_back(2); });
        std::move(*first).complete([&delivered]() noexcept { delivered.push_back(1); });
    }
    while (io->poll() > 0) {
    }
    CHECK(delivered == std::vector<int>{1, 2});
    root.close();
}

TEST_CASE(
    "RuntimeRoot close drains admitted worker completions before teardown",
    "[harness][runtime][issue459]") {
    auto io = std::make_shared<boost::asio::io_context>();
    harness::RuntimeRoot root(io, 2, 8, 1024 * 1024);
    auto target = root.make_target();

    std::atomic<int> completed{0};
    {
        auto first = target->try_admit(4);
        auto second = target->try_admit(4);
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        REQUIRE(first->post_worker([]() noexcept {}));
        REQUIRE(second->post_worker([]() noexcept {}));
        std::move(*first).complete([&completed]() noexcept { completed.fetch_add(1); });
        std::move(*second).complete([&completed]() noexcept { completed.fetch_add(1); });
    }
    // Close stops admission, drains queued worker work, and joins workers; the
    // caller still pumps the loop so every admitted terminal arrives.
    root.close();
    CHECK_FALSE(target->try_admit(4).has_value());
    while (io->poll() > 0) {
    }
    CHECK(completed.load() == 2);
}
