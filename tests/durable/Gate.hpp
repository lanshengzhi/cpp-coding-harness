#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>

namespace cch::tests::durable {

/// One release gate a durable test tool or phase waits on. A phase waits with
/// its stop token, so an aborted or closed Harness releases it; the bounded
/// `wait_for` keeps a teardown join from hanging when no token exists
/// (CODING_STANDARDS.md §11.10).
struct Gate {
    // `condition_variable_any` carries the stop-token wait overloads.
    std::mutex mutex;
    std::condition_variable_any cv;
    bool open{false};

    void release() {
        {
            std::lock_guard lock(mutex);
            open = true;
        }
        cv.notify_all();
    }

    /// Returns true when released, false when the invocation was stopped.
    [[nodiscard]] bool wait(const std::stop_token& token) {
        std::unique_lock lock(mutex);
        return cv.wait(lock, token, [this] { return open; });
    }

    /// Bounded variant for waits that have no stop token: a failed assertion
    /// before the release must not hang the teardown join (§11.10).
    [[nodiscard]] bool wait_for(std::chrono::seconds budget) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, budget, [this] { return open; });
    }
};

} // namespace cch::tests::durable
