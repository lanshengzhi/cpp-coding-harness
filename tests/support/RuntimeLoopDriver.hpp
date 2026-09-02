#pragma once

#include "support/RuntimeFixture.hpp"

#include <atomic>
#include <chrono>
#include <thread>

namespace cch::tests {

/// Drive a RuntimeFixture's interaction loop on a helper thread while a test
/// exercises a Session through another synchronous or asynchronous seam. The
/// fixture remains the owner of the loop and RuntimeRoot; this driver only
/// keeps the loop serviced until it is destroyed. The fixture is borrowed:
/// it must outlive this driver (declare the driver after the fixture).
///
/// The driver services the loop directly instead of routing through
/// RuntimeFixture::run: run() carries a bounded wait budget that fails the
/// test when a long-lived keep-alive operation is still pending, which is the
/// driver's normal state for any test body that outlives the budget.
/// RuntimeRoot holds a work guard until close(), so run_one() blocks for
/// work at full speed rather than busy-polling; destruction stops the loop
/// to release a blocked run_one().
class RuntimeLoopDriver final {
public:
    explicit RuntimeLoopDriver(RuntimeFixture& runtime) : runtime_(&runtime), thread_([this] { drive(); }) {
        while (!started_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

    ~RuntimeLoopDriver() {
        stop_.store(true, std::memory_order_release);
        runtime_->loop().stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    RuntimeLoopDriver(const RuntimeLoopDriver&) = delete;
    RuntimeLoopDriver& operator=(const RuntimeLoopDriver&) = delete;
    RuntimeLoopDriver(RuntimeLoopDriver&&) = delete;
    RuntimeLoopDriver& operator=(RuntimeLoopDriver&&) = delete;

private:
    void drive() noexcept {
        started_.store(true, std::memory_order_release);
        auto& loop = runtime_->loop();
        while (!stop_.load(std::memory_order_acquire)) {
            if (loop.stopped()) {
                loop.restart();
            }
            // Bounded so a stop() racing the restart above cannot leave a
            // blocked run_one() past the destructor's join.
            (void)loop.run_one_for(std::chrono::milliseconds{50});
        }
    }

    RuntimeFixture* runtime_;
    std::atomic<bool> started_{false};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace cch::tests
