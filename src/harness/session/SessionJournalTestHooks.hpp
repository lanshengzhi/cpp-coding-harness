#pragma once

#include <cstddef>
#include <filesystem>

namespace cch::harness::session::testing {

/// Fail one selected append attempt for a single session path. `attempt` is
/// one-based and counted from this call. The injection is consumed on failure.
/// This private hook exists only for persistence-failure behavior tests.
void fail_nth_append_for_test(const std::filesystem::path& path, std::size_t attempt);

} // namespace cch::harness::session::testing
