#include "support/AllocationCounter.hpp"

#include <atomic>
#include <cstdlib>
#include <exception>
#include <new>

namespace {

std::atomic<std::size_t> g_allocations{0};

} // namespace

namespace cch::tests {

std::size_t allocation_count() noexcept {
    return g_allocations.load(std::memory_order_relaxed);
}

void reset_allocation_count() noexcept {
    g_allocations.store(0, std::memory_order_relaxed);
}

} // namespace cch::tests

void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size)) {
        return memory;
    }
    // Test-only OOM fallback: the strict no-exception build (ADR 0042) has no
    // throw channel, and allocation failure on this counting override is an
    // unrecoverable invariant, so terminate.
    std::terminate();
}

void* operator new[](std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size)) {
        return memory;
    }
    std::terminate();
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}
