#include "support/AllocationCounter.hpp"

#include <atomic>
#include <cstdlib>
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
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size)) {
        return memory;
    }
    throw std::bad_alloc();
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
