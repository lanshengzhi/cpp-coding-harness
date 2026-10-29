#pragma once

#include <cch/support/AsyncResult.hpp>

#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>

namespace cch::durable {

/// Wait only on a Harness task's execution thread, never on the Session line
/// or an event loop. The wait retains all borrowed callback locals through
/// completion; callers keep the operation's other borrowed inputs alive.
template <typename T> support::Expected<T> await_result(support::AsyncResult<T> operation) {
    std::mutex mutex;
    std::condition_variable ready;
    std::optional<support::Expected<T>> outcome;
    std::move(operation).start([&mutex, &ready, &outcome](support::Expected<T> result) noexcept {
        std::lock_guard lock(mutex);
        outcome.emplace(std::move(result));
        ready.notify_one();
    });
    std::unique_lock lock(mutex);
    ready.wait(lock, [&outcome] { return outcome.has_value(); });
    return std::move(*outcome);
}

} // namespace cch::durable
