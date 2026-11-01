#include "alloc_hook.hpp"
#include <telemetry/mem_collector.hpp>

#include <cassert>
#include <cmath>
#include <iostream>
#include <fcntl.h>
#include <cerrno>
#include <unistd.h>

// ==============================================================================
// Unit Test: MemCollector Zero-Allocation Verification
// ==============================================================================

// Synthetic Input Validation: Test the parser logic against fixed string literals
// representing mock /proc/meminfo contents to verify exact arithmetic correctness.
static void test_mem_synthetic_parsing() {
    std::cout << "[RUN] test_mem_synthetic_parsing..." << std::endl;

    constexpr std::string_view mock_meminfo = 
        "MemTotal:       16000000 kB\n"
        "MemFree:         2000000 kB\n"
        "MemAvailable:    8000000 kB\n"
        "Buffers:          500000 kB\n"
        "Cached:          4000000 kB\n"
        "SwapCached:        12345 kB\n"
        "Active:          6000000 kB\n"
        "Inactive:        3000000 kB\n"
        "Dirty:              1000 kB\n"
        "SwapTotal:       4000000 kB\n"
        "SwapFree:        3000000 kB\n"
        "Shmem:            200000 kB\n";

    telemetry::MemSnapshot snap{};
    bool ok = telemetry::parse_proc_meminfo(mock_meminfo, snap);
    assert(ok);

    // Exact field extraction assertions
    assert(snap.mem_total == 16000000);
    assert(snap.mem_free == 2000000);
    assert(snap.mem_available == 8000000);
    assert(snap.buffers == 500000);
    assert(snap.cached == 4000000);
    assert(snap.active == 6000000);
    assert(snap.inactive == 3000000);
    assert(snap.dirty == 1000);
    assert(snap.swap_total == 4000000);
    assert(snap.swap_free == 3000000);

    // Modern memory arithmetic: Used = MemTotal - MemAvailable = 16,000,000 - 8,000,000 = 8,000,000 kB
    assert(snap.used() == 8000000);

    // Active Utilization % = (8,000,000 / 16,000,000) * 100 = 50.0%
    assert(std::abs(snap.utilization_percent() - 50.0) < 0.0001);

    // Swap Used = SwapTotal - SwapFree = 4,000,000 - 3,000,000 = 1,000,000 kB
    assert(snap.swap_used() == 1000000);

    // Test fallback when MemAvailable is 0 (older kernels)
    telemetry::MemSnapshot legacy_snap = snap;
    legacy_snap.mem_available = 0;
    // Fallback Used = MemTotal - MemFree - (Buffers + Cached)
    // = 16,000,000 - 2,000,000 - (500,000 + 4,000,000) = 9,500,000 kB
    assert(legacy_snap.used() == 9500000);
    // Fallback Utilization % = (9,500,000 / 16,000,000) * 100 = 59.375%
    assert(std::abs(legacy_snap.utilization_percent() - 59.375) < 0.0001);

    std::cout << "  [PASS] test_mem_synthetic_parsing exact arithmetic verified!" << std::endl;
}

static void test_mem_collector_basic() {
    std::cout << "[RUN] test_mem_collector_basic..." << std::endl;

    int recorded_fd = -1;
    {
        telemetry::MemCollector collector;
        recorded_fd = collector.fd();
        assert(recorded_fd >= 0);
        assert(::fcntl(recorded_fd, F_GETFD) != -1); // Valid open descriptor

        bool ok = collector.sample();
        assert(ok);

        const auto& snap = collector.snapshot();
        // System must have non-zero physical memory
        assert(snap.mem_total > 0);
        assert(snap.mem_free > 0);
        assert(snap.used() > 0);
        assert(snap.used() <= snap.mem_total);

        const double pct = snap.utilization_percent();
        assert(pct >= 0.0 && pct <= 100.0);

        std::cout << "  [INFO] Total RAM: " << snap.mem_total << " KiB, Used: " << snap.used() 
                  << " KiB, Utilization: " << pct << "%" << std::endl;
    }

    // Verify zero unclosed file descriptors: descriptor must be closed cleanly upon destruction!
    assert(::fcntl(recorded_fd, F_GETFD) == -1);
    assert(errno == EBADF);

    std::cout << "  [PASS] test_mem_collector_basic verified (descriptor closed cleanly)!" << std::endl;
}

static void test_mem_collector_zero_alloc_1000_iterations() {
    std::cout << "[RUN] test_mem_collector_zero_alloc_1000_iterations..." << std::endl;

    telemetry::MemCollector collector;
    bool ok = collector.sample();
    assert(ok);

    // Strict zero-allocation surveillance across 1,000 consecutive sampling ticks
    {
        AllocGuard guard;

        for (int i = 0; i < 1000; ++i) {
            ok = collector.sample();
            assert(ok);

            const auto& snap = collector.snapshot();
            const uint64_t used = snap.used();
            const double pct = snap.utilization_percent();

            assert(used <= snap.mem_total);
            assert(pct >= 0.0 && pct <= 100.0);
        }

        // Mechanically verify that strictly ZERO heap allocations occurred across all 1,000 iterations
        guard.assert_zero_allocations();
    }

    std::cout << "  [PASS] test_mem_collector_zero_alloc_1000_iterations passed with 0 allocations!" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Running test_mem harness..." << std::endl;
    std::cout << "========================================" << std::endl;

    test_mem_synthetic_parsing();
    test_mem_collector_basic();
    test_mem_collector_zero_alloc_1000_iterations();

    std::cout << "========================================" << std::endl;
    std::cout << "All test_mem checks passed successfully!" << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}

