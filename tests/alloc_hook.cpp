#include "alloc_hook.hpp"

#include <cstdlib>
#include <new>
#include <unistd.h>

// ==============================================================================
// Global Memory Allocation Interceptor - Definitions
// ==============================================================================

// Global atomic counters tracking cumulative allocations/deallocations across all threads
std::atomic<std::size_t> g_allocation_count{0};
std::atomic<std::size_t> g_deallocation_count{0};

// Per-thread trap flag (defaults to false so setup/test initialization can allocate freely)
thread_local bool t_trap_on_allocation{false};

// Helper internal function to handle the allocation accounting & trap check
// static => "Private to this file."
static void* record_and_allocate(std::size_t size) {
    if (t_trap_on_allocation) {
        // Direct POSIX write syscall to STDERR_FILENO.
        // We do NOT use std::cerr or printf because standard I/O library streams
        // can allocate heap buffers or mutate internal stream mutex locks!
        const char msg[] = "[FATAL] Rogue heap allocation intercepted in zero-alloc section!\n";
        // Silence compiler warnings when we intentionally ignore the return value of write() which is marked "Do not ignore this return value!"
        (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
        std::abort();
    }
    // std::memory_order_relaxed => lowest possible CPU overhead
    g_allocation_count.fetch_add(1, std::memory_order_relaxed);

    // In C++, malloc(0) is implementation-defined, but operator new(0) must return non-null
    if (size == 0) {
        size = 1;
    }
    return std::malloc(size);
}

// Helper internal function to handle deallocation accounting
static void record_and_deallocate(void* ptr) noexcept {
    if (ptr != nullptr) {
        g_deallocation_count.fetch_add(1, std::memory_order_relaxed);
        std::free(ptr);
    }
}

// ==============================================================================
// Overloaded Primary Replacement Signatures
// ==============================================================================

// 1. Single-object allocation
void* operator new(std::size_t size) {
    void* ptr = record_and_allocate(size);
    if (!ptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

// 2. Array allocation
void* operator new[](std::size_t size) {
    void* ptr = record_and_allocate(size);
    if (!ptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

// 3. Single-object nothrow allocation
// nothrow => Returns nullptr if allocation fails!
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return record_and_allocate(size);
}

// 4. Array nothrow allocation
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return record_and_allocate(size);
}

// 5. Symmetric delete variants
void operator delete(void* ptr) noexcept {
    record_and_deallocate(ptr);
}

void operator delete[](void* ptr) noexcept {
    record_and_deallocate(ptr);
}

// C++14 / C++17 sized-delete variants
void operator delete(void* ptr, std::size_t) noexcept {
    record_and_deallocate(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
    record_and_deallocate(ptr);
}

// nothrow delete variants
void operator delete(void* ptr, const std::nothrow_t&) noexcept {
    record_and_deallocate(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
    record_and_deallocate(ptr);
}
