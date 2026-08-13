#pragma once

#include <cstddef>

namespace cch::tests {

/// Global allocation counter backing a test-local `operator new` override.
/// The override lives in AllocationCounter.cpp and is compiled only into the
/// support test shard, where it measures the ready-path no-allocation contract
/// of `cch::support::AsyncResult`.
[[nodiscard]] std::size_t allocation_count() noexcept;
void reset_allocation_count() noexcept;

} // namespace cch::tests
