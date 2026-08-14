#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <thread>
#include <vector>

namespace cch::harness::session::testing {

/// Fail one selected append attempt for a single session path. `attempt` is
/// one-based and counted from this call. The injection is consumed on failure.
/// This private hook exists only for persistence-failure behavior tests.
void fail_nth_append_for_test(const std::filesystem::path& path, std::size_t attempt);

/// Delay every append for a single session path by `delay` until cleared.
/// This private hook exists only for slow-persistence behavior tests.
void delay_appends_for_test(
    const std::filesystem::path& path,
    std::chrono::milliseconds delay);
void clear_append_delay_for_test(const std::filesystem::path& path);

/// Record the executing thread id of every append for a single session path
/// until `recorded_append_threads_for_test` collects (and clears) them. This
/// private hook exists only for off-loop persistence proof tests.
void record_append_threads_for_test(const std::filesystem::path& path);
[[nodiscard]] std::vector<std::thread::id> recorded_append_threads_for_test(
    const std::filesystem::path& path);

} // namespace cch::harness::session::testing
