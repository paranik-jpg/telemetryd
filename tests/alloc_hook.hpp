#pragma once

#include <atomic>
#include <cstddef>

// ==============================================================================
// Global Memory Allocation Interceptor (Accounting Primitives)
// ==============================================================================

// Global atomic counters tracking every heap allocation and deallocation
// extern => Tells any file including alloc_hook.hpp that these variables exist!
// => No storage created here, just name and type

// std::atomic => Guarantees safe, accurate counting across CPU cores without mutex locks.
extern std::atomic<std::size_t> g_allocation_count;
extern std::atomic<std::size_t> g_deallocation_count;

// Per-thread flag: when set to true, any heap allocation attempt on this thread
// immediately triggers a fatal abort / assertion failure.
// thread_local => Every individual thread gets its own independent copy of this variable.
extern thread_local bool t_trap_on_allocation;

// ==============================================================================
// RAII Scoped Alloc Guard (AllocGuard)
// ==============================================================================

#include <cassert>

// Just like mutex places a block of code under concurrency surveillance:
// Our AllocGuard works in the exact same way, but for zero-allocation surveillance:
struct AllocGuard {
    std::size_t baseline_allocations{0};
    std::size_t baseline_deallocations{0};
    bool previous_trap_state{false};

    // Constructor: saves state, activates trap on this thread, and snapshots baseline counts
    AllocGuard() noexcept
        : baseline_allocations(g_allocation_count.load(std::memory_order_relaxed)),
          baseline_deallocations(g_deallocation_count.load(std::memory_order_relaxed)),
          previous_trap_state(t_trap_on_allocation)
    {
        t_trap_on_allocation = true;
    }

    // Destructor: restores the previous trap state when leaving scope
    ~AllocGuard() noexcept {
        t_trap_on_allocation = previous_trap_state;
    }

    // Non-copyable, non-movable to ensure strict RAII scope discipline
    AllocGuard(const AllocGuard&) = delete;
    AllocGuard& operator=(const AllocGuard&) = delete;
    AllocGuard(AllocGuard&&) = delete;
    AllocGuard& operator=(AllocGuard&&) = delete;

    // Verifies that strictly zero heap allocations occurred during this guard's lifetime
    void assert_zero_allocations() const noexcept {
        const std::size_t current_allocations = g_allocation_count.load(std::memory_order_relaxed);
        assert((current_allocations - baseline_allocations) == 0);
    }
};
