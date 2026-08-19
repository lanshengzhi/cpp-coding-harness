#include "agent/harness/RuntimeRoot.hpp"
#include "support/PumpUntil.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

using namespace cch;

namespace {

using tests::pump_until;

} // namespace

TEST_CASE(
    "RuntimeRoot rejects admission at operation-count saturation without running work",
    "[harness][runtime][issue459]") {
    auto io = std::make_shared<boost::asio::io_context>();
    harness::RuntimeRoot root(
        io,
        harness::RuntimeLimits{
            .worker_count = 1,
            .max_admitted_operations = 2,
            .max_admitted_bytes = 1024 * 1024,
        });
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
    harness::RuntimeRoot root(
        io,
        harness::RuntimeLimits{
            .worker_count = 1,
            .max_admitted_operations = 8,
            .max_admitted_bytes = 100,
        });
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
    harness::RuntimeRoot root(
        io,
        harness::RuntimeLimits{
            .worker_count = 1,
            .max_admitted_operations = 4,
            .max_admitted_bytes = 1024 * 1024,
        });
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
    harness::RuntimeRoot root(
        io,
        harness::RuntimeLimits{
            .worker_count = 2,
            .max_admitted_operations = 8,
            .max_admitted_bytes = 1024 * 1024,
        });
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
    harness::RuntimeRoot root(
        io,
        harness::RuntimeLimits{
            .worker_count = 2,
            .max_admitted_operations = 8,
            .max_admitted_bytes = 1024 * 1024,
        });
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

TEST_CASE(
    "a busy target's mailbox drain requeues in bounded batches so a second target is not starved",
    "[harness][runtime][issue465]") {
    auto io = std::make_shared<boost::asio::io_context>();
    harness::RuntimeRoot root(
        io,
        harness::RuntimeLimits{
            .worker_count = 2,
            .max_admitted_operations = 128,
            .max_admitted_bytes = 1024 * 1024,
            .mailbox_drain_batch = 2,
        });
    auto busy_target = root.make_target();
    auto peer_target = root.make_target();

    std::atomic<int> busy_delivered{0};
    std::atomic<int> peer_delivered_at_probe{0};
    std::atomic<bool> peer_delivered{false};

    // One target accumulates a deep completion backlog; its drain must yield
    // after each bounded batch. A peer target's single terminal is posted
    // after the backlog, so with an unbounded drain it would wait for the
    // whole backlog; with bounded batches it is serviced after at most one
    // batch of the busy target.
    std::vector<harness::RuntimeTarget::Admission> backlog;
    for (std::size_t index = 0; index < 64; ++index) {
        auto admission = busy_target->try_admit(4);
        REQUIRE(admission.has_value());
        std::move(*admission).complete(
            [&busy_delivered]() noexcept {
                busy_delivered.fetch_add(1, std::memory_order_release);
            });
    }
    auto peer_admission = peer_target->try_admit(4);
    REQUIRE(peer_admission.has_value());
    std::move(*peer_admission).complete([&]() noexcept {
        peer_delivered_at_probe.store(
            busy_delivered.load(std::memory_order_acquire),
            std::memory_order_release);
        peer_delivered.store(true, std::memory_order_release);
    });

    // Poll until the peer terminal arrives and the busy backlog empties
    // (bounded budget so a regression cannot hang the suite).
    REQUIRE(pump_until(
        *io,
        [&] {
            return peer_delivered.load(std::memory_order_acquire) &&
                busy_delivered.load(std::memory_order_acquire) == 64;
        }));
    root.close();

    // The peer terminal ran after at most one bounded batch of the busy
    // target's backlog, never after the whole backlog.
    CHECK(peer_delivered.load(std::memory_order_acquire));
    CHECK(peer_delivered_at_probe.load(std::memory_order_acquire) > 0);
    CHECK(peer_delivered_at_probe.load(std::memory_order_acquire) <= 2);
    // Every admitted terminal still delivered exactly once.
    CHECK(busy_delivered.load(std::memory_order_acquire) == 64);
}

TEST_CASE(
    "a loop-posted Close signal is serviced between bounded mailbox batches",
    "[harness][runtime][issue465]") {
    auto io = std::make_shared<boost::asio::io_context>();
    harness::RuntimeRoot root(
        io,
        harness::RuntimeLimits{
            .worker_count = 2,
            .max_admitted_operations = 32,
            .max_admitted_bytes = 1024 * 1024,
            .mailbox_drain_batch = 4,
        });
    auto target = root.make_target();

    std::atomic<int> delivered{0};
    // Slow deliveries model sustained model/render traffic: the drain stays
    // busy long enough that the loop must yield between batches for the Close
    // control task to make progress.
    for (std::size_t index = 0; index < 16; ++index) {
        auto admission = target->try_admit(4);
        REQUIRE(admission.has_value());
        std::move(*admission).complete([&delivered]() noexcept {
            delivered.fetch_add(1, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        });
    }
    std::atomic<int> delivered_at_close{0};
    std::atomic<bool> close_processed{false};
    boost::asio::post(*io, [&]() noexcept {
        delivered_at_close.store(delivered.load(std::memory_order_acquire));
        close_processed.store(true, std::memory_order_release);
    });

    // The Close control task ran at a batch boundary, not after the backlog:
    // sustained traffic cannot starve Close progress.
    REQUIRE(pump_until(
        *io,
        [&] {
            return close_processed.load(std::memory_order_acquire) &&
                delivered.load(std::memory_order_acquire) == 16;
        }));
    root.close();

    CHECK(close_processed.load(std::memory_order_acquire));
    CHECK(delivered_at_close.load(std::memory_order_acquire) > 0);
    CHECK(delivered_at_close.load(std::memory_order_acquire) < 16);
    CHECK(delivered.load(std::memory_order_acquire) == 16);
}

TEST_CASE(
    "reserved control admission succeeds while the ordinary budget is exhausted",
    "[harness][runtime][issue465]") {
    auto io = std::make_shared<boost::asio::io_context>();
    harness::RuntimeRoot root(
        io,
        harness::RuntimeLimits{
            .worker_count = 1,
            .max_admitted_operations = 2,
            .max_admitted_bytes = 1024 * 1024,
            .max_reserved_operations = 4,
            .max_reserved_bytes = 1024 * 1024,
        });
    auto target = root.make_target();

    // Fill the ordinary budget with bulk work.
    auto first = target->try_admit(4);
    auto second = target->try_admit(4);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK_FALSE(target->try_admit(4).has_value());

    // Control work (persistence, credential, terminal completion, Close)
    // draws from the reserved lane and cannot be rejected behind the full
    // ordinary budget.
    std::vector<harness::RuntimeTarget::Admission> control;
    for (std::size_t index = 0; index < 4; ++index) {
        auto admission = target->try_admit_reserved(4);
        REQUIRE(admission.has_value());
        control.push_back(std::move(*admission));
    }
    CHECK_FALSE(target->try_admit_reserved(4).has_value());

    std::move(*first).complete([]() noexcept {});
    std::move(*second).complete([]() noexcept {});
    for (auto& admission : control) {
        std::move(admission).complete([]() noexcept {});
    }
    while (io->poll() > 0) {
    }

    // Both lanes released: the ordinary budget accepts new bulk work again.
    CHECK(target->try_admit(4).has_value());
    root.close();
}

TEST_CASE(
    "reserved admission enforces an independent byte budget",
    "[harness][runtime][issue465]") {
    auto io = std::make_shared<boost::asio::io_context>();
    harness::RuntimeRoot root(
        io,
        harness::RuntimeLimits{
            .worker_count = 1,
            .max_admitted_operations = 8,
            .max_admitted_bytes = 100,
            .max_reserved_operations = 8,
            .max_reserved_bytes = 40,
        });
    auto target = root.make_target();

    auto ordinary = target->try_admit(60);
    REQUIRE(ordinary.has_value());
    // Ordinary byte budget is independent: 60 + 60 > 100 rejects.
    CHECK_FALSE(target->try_admit(60).has_value());
    // Reserved byte budget is independent and much smaller: 30 + 30 > 40.
    auto control = target->try_admit_reserved(30);
    REQUIRE(control.has_value());
    CHECK_FALSE(target->try_admit_reserved(30).has_value());
    // Ordinary admission is still available while the reserved byte budget is
    // exhausted.
    auto more_ordinary = target->try_admit(40);
    REQUIRE(more_ordinary.has_value());

    std::move(*ordinary).complete([]() noexcept {});
    std::move(*control).complete([]() noexcept {});
    std::move(*more_ordinary).complete([]() noexcept {});
    while (io->poll() > 0) {
    }
    root.close();
}

TEST_CASE(
    "control and ordinary terminals deliver through one mailbox in admission order",
    "[harness][runtime][issue465]") {
    auto io = std::make_shared<boost::asio::io_context>();
    harness::RuntimeRoot root(
        io,
        harness::RuntimeLimits{
            .worker_count = 2,
            .max_admitted_operations = 16,
            .max_admitted_bytes = 1024 * 1024,
            .max_reserved_operations = 8,
            .max_reserved_bytes = 1024 * 1024,
        });
    auto target = root.make_target();

    std::vector<int> delivered;
    {
        auto ordinary_a = target->try_admit(4);
        auto control_a = target->try_admit_reserved(4);
        auto control_b = target->try_admit_reserved(4);
        auto ordinary_b = target->try_admit(4);
        REQUIRE(ordinary_a.has_value());
        REQUIRE(control_a.has_value());
        REQUIRE(control_b.has_value());
        REQUIRE(ordinary_b.has_value());

        // Complete in reverse admission order; delivery must follow the single
        // per-target sequence across both lanes (1,2,3,4).
        std::move(*ordinary_b).complete([&delivered]() noexcept { delivered.push_back(4); });
        std::move(*control_b).complete([&delivered]() noexcept { delivered.push_back(3); });
        std::move(*control_a).complete([&delivered]() noexcept { delivered.push_back(2); });
        std::move(*ordinary_a).complete([&delivered]() noexcept { delivered.push_back(1); });
    }
    while (io->poll() > 0) {
    }
    CHECK(delivered == std::vector<int>{1, 2, 3, 4});
    root.close();
}

TEST_CASE(
    "timers keep firing while sustained traffic fills a production-capacity mailbox",
    "[harness][runtime][issue465]") {
    // The measured production policy (docs/runtime-capacities.md): the
    // default RuntimeLimits is the chosen worker/admission/batch policy.
    auto io = std::make_shared<boost::asio::io_context>();
    harness::RuntimeRoot root(io, harness::RuntimeLimits{});
    auto target = root.make_target();

    std::atomic<bool> keep_going{true};
    std::atomic<std::size_t> delivered{0};
    std::atomic<std::size_t> admitted{0};

    // Sustained Agent/model/tool traffic: a producer keeps the mailbox
    // saturated at the production admission budget by completing fresh
    // operations as capacity frees, so the drain never catches a quiet gap.
    std::jthread producer([&] {
        while (keep_going.load(std::memory_order_acquire)) {
            auto admission = target->try_admit(4);
            if (!admission) {
                std::this_thread::sleep_for(std::chrono::microseconds{10});
                continue;
            }
            admitted.fetch_add(1, std::memory_order_release);
            std::move(*admission).complete([&delivered]() noexcept {
                delivered.fetch_add(1, std::memory_order_release);
            });
        }
    });

    // A repeating 5ms timer must keep firing while traffic is sustained; an
    // unbounded drain would never return to the loop and would starve it.
    std::atomic<int> timer_fires{0};
    std::atomic<bool> fired_while_sustained{false};
    std::function<void()> arm_timer;
    arm_timer = [&] {
        auto timer = std::make_shared<boost::asio::steady_timer>(
            *io, std::chrono::milliseconds{5});
        timer->async_wait([&, timer](const boost::system::error_code&) {
            timer_fires.fetch_add(1, std::memory_order_release);
            if (keep_going.load(std::memory_order_acquire)) {
                fired_while_sustained.store(true, std::memory_order_release);
                arm_timer();
            }
        });
    };
    arm_timer();

    // Let sustained traffic run for a measured window.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds{200};
    while (std::chrono::steady_clock::now() < deadline) {
        if (io->stopped()) {
            io->restart();
        }
        (void)io->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    keep_going.store(false, std::memory_order_release);

    // Drain whatever remains admitted; every admitted terminal delivers once.
    REQUIRE(pump_until(
        *io,
        [&] { return delivered.load(std::memory_order_acquire) ==
                    admitted.load(std::memory_order_acquire); },
        std::chrono::milliseconds{5000}));
    root.close();

    // The repeating timer fired repeatedly, including while traffic was
    // still sustained: bounded batches keep timers progressing.
    CHECK(timer_fires.load(std::memory_order_acquire) >= 2);
    CHECK(fired_while_sustained.load(std::memory_order_acquire));
    CHECK(delivered.load(std::memory_order_acquire) ==
          admitted.load(std::memory_order_acquire));
    CHECK(admitted.load(std::memory_order_acquire) > 0);
}

TEST_CASE(
    "Close control work progresses while a production-capacity mailbox drains sustained traffic",
    "[harness][runtime][issue465]") {
    auto io = std::make_shared<boost::asio::io_context>();
    harness::RuntimeRoot root(io, harness::RuntimeLimits{});
    auto target = root.make_target();

    std::atomic<int> delivered{0};
    for (std::size_t index = 0; index < 32; ++index) {
        auto admission = target->try_admit(4);
        REQUIRE(admission.has_value());
        std::move(*admission).complete([&delivered]() noexcept {
            delivered.fetch_add(1, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        });
    }

    // The application Close control path posts to the loop; it must be
    // serviced between bounded batches, not after the whole backlog.
    std::atomic<int> delivered_at_close{0};
    std::atomic<bool> close_processed{false};
    boost::asio::post(*io, [&]() noexcept {
        delivered_at_close.store(delivered.load(std::memory_order_acquire));
        close_processed.store(true, std::memory_order_release);
    });

    REQUIRE(pump_until(
        *io,
        [&] {
            return close_processed.load(std::memory_order_acquire) &&
                delivered.load(std::memory_order_acquire) == 32;
        }));
    // Close stops admission and joins workers while the caller keeps pumping
    // so every admitted terminal reaches its mailbox.
    root.close();
    while (io->poll() > 0) {
    }

    CHECK(close_processed.load(std::memory_order_acquire));
    CHECK(delivered_at_close.load(std::memory_order_acquire) > 0);
    CHECK(delivered_at_close.load(std::memory_order_acquire) < 32);
    CHECK(delivered.load(std::memory_order_acquire) == 32);
    CHECK_FALSE(target->try_admit(4).has_value());
}

TEST_CASE(
    "RuntimeRoot Close releases the loop work guard so the final application drain returns",
    "[harness][runtime][close][issue467]") {
    auto io = std::make_shared<boost::asio::io_context>();
    harness::RuntimeRoot root(
        io,
        harness::RuntimeLimits{
            .worker_count = 2,
            .max_admitted_operations = 8,
            .max_admitted_bytes = 1024 * 1024,
        });
    auto target = root.make_target();

    std::vector<int> delivered;
    {
        auto first = target->try_admit(4);
        auto second = target->try_admit(4);
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        std::move(*first).complete(
            [&delivered]() noexcept { delivered.push_back(1); });
        std::move(*second).complete(
            [&delivered]() noexcept { delivered.push_back(2); });
    }

    // Final application Close (ADR 0040): stop admission and join the
    // workers, then pump the loop so every admitted terminal reaches its
    // mailbox.
    root.close();
    while (io->poll() > 0) {
    }
    CHECK(delivered == std::vector<int>{1, 2});

    // The guard is released: an empty loop returns instead of blocking, so
    // the application drain after Close terminates deterministically.
    CHECK(io->run() == 0);
}

TEST_CASE(
    "RuntimeRoot Close under load delivers every admitted terminal in per-target order",
    "[harness][runtime][close][issue467]") {
    auto io = std::make_shared<boost::asio::io_context>();
    constexpr std::size_t kOperationsPerTarget = 32;
    harness::RuntimeRoot root(
        io,
        harness::RuntimeLimits{
            .worker_count = 2,
            .max_admitted_operations = 2 * kOperationsPerTarget,
            .max_admitted_bytes = 1024 * 1024,
        });
    auto first_target = root.make_target();
    auto second_target = root.make_target();

    std::vector<int> first_delivered;
    std::vector<int> second_delivered;
    const auto admit_worker =
        [](const std::shared_ptr<harness::RuntimeTarget>& target,
           std::vector<int>& delivered,
           int marker) {
            auto admission = target->try_admit(4);
            REQUIRE(admission.has_value());
            auto held = std::make_shared<harness::RuntimeTarget::Admission>(
                std::move(*admission));
            REQUIRE(held->post_worker(
                [held, &delivered, marker]() mutable noexcept {
                    std::move(*held).complete(
                        [&delivered, marker]() noexcept {
                            delivered.push_back(marker);
                        });
                }));
        };
    for (std::size_t index = 0; index < kOperationsPerTarget; ++index) {
        admit_worker(first_target, first_delivered, static_cast<int>(index));
        admit_worker(second_target, second_delivered, static_cast<int>(index));
    }

    // Close under load: admission stops, queued worker work drains, workers
    // join, and the caller's pump delivers every admitted terminal through
    // the per-target mailboxes in admission order.
    root.close();
    CHECK_FALSE(first_target->try_admit(4).has_value());
    CHECK_FALSE(second_target->try_admit(4).has_value());
    while (io->poll() > 0) {
    }

    const auto expected = [] {
        std::vector<int> markers;
        for (std::size_t index = 0; index < kOperationsPerTarget; ++index) {
            markers.push_back(static_cast<int>(index));
        }
        return markers;
    }();
    CHECK(first_delivered == expected);
    CHECK(second_delivered == expected);
}
