#include "alloc_hook.hpp"
#include <telemetry/cpu_collector.hpp>

#include <cassert>
#include <cmath>
#include <iostream>
#include <fcntl.h>
#include <cerrno>
#include <unistd.h>

// ==============================================================================
// Unit Test: CpuCollector Zero-Allocation Verification
// ==============================================================================

// Synthetic Input Validation: Test the parser logic against fixed string literals
// representing mock /proc/stat contents to verify exact arithmetic correctness against known jiffies.
static void test_cpu_synthetic_parsing() {
    std::cout << "[RUN] test_cpu_synthetic_parsing..." << std::endl;

    constexpr std::string_view mock_tick_1 =
        "cpu  100 20 50 800 10 5 3 2 0 0\n"
        "cpu0 60 10 30 400 5 3 2 1 0 0\n"
        "cpu1 40 10 20 400 5 2 1 1 0 0\n"
        "intr 123456 78 90\n"
        "ctxt 998877\n";

    constexpr std::string_view mock_tick_2 =
        "cpu  150 30 70 850 15 7 5 3 0 0\n"
        "cpu0 90 15 40 425 8 4 3 2 0 0\n"
        "cpu1 60 15 30 425 7 3 2 1 0 0\n"
        "intr 123999 80 92\n"
        "ctxt 999900\n";

    telemetry::CpuCollectorState state{};

    // Parse Tick 1
    bool ok = telemetry::parse_proc_stat(mock_tick_1, state);
    assert(ok);
    assert(state.detected_cores == 2); // 2 individual cores (cpu0, cpu1)

    // Check parsed values for aggregate cpu
    const auto& agg1 = state.m_current[0];
    assert(agg1.user == 100);
    assert(agg1.nice == 20);
    assert(agg1.system == 50);
    assert(agg1.idle == 800);
    assert(agg1.iowait == 10);
    assert(agg1.irq == 5);
    assert(agg1.softirq == 3);
    assert(agg1.steal == 2);
    assert(agg1.total() == 990);
    assert(agg1.active() == 180);

    // Check parsed values for cpu0 (index 1)
    const auto& c0_1 = state.m_current[1];
    assert(c0_1.user == 60);
    assert(c0_1.nice == 10);
    assert(c0_1.system == 30);
    assert(c0_1.idle == 400);
    assert(c0_1.iowait == 5);
    assert(c0_1.irq == 3);
    assert(c0_1.softirq == 2);
    assert(c0_1.steal == 1);
    assert(c0_1.total() == 511);
    assert(c0_1.active() == 106);

    // Check parsed values for cpu1 (index 2)
    const auto& c1_1 = state.m_current[2];
    assert(c1_1.user == 40);
    assert(c1_1.nice == 10);
    assert(c1_1.system == 20);
    assert(c1_1.idle == 400);
    assert(c1_1.iowait == 5);
    assert(c1_1.irq == 2);
    assert(c1_1.softirq == 1);
    assert(c1_1.steal == 1);
    assert(c1_1.total() == 479);
    assert(c1_1.active() == 74);

    // Shift tick 1 to previous, then parse tick 2 into current
    state.m_previous = state.m_current;
    ok = telemetry::parse_proc_stat(mock_tick_2, state);
    assert(ok);

    const auto& agg2 = state.m_current[0];
    assert(agg2.total() == 1130);
    assert(agg2.active() == 265);

    // Delta Utilization Arithmetic Verification:
    // delta_total  = 1130 - 990 = 140
    // delta_active = 265 - 180 = 85
    // usage % = (85 / 140) * 100 = 60.714285714%
    const double agg_util = telemetry::compute_cpu_utilization(state.m_previous[0], state.m_current[0]);
    assert(std::abs(agg_util - (85.0 / 140.0 * 100.0)) < 0.0001);

    // cpu0 delta arithmetic:
    // prev: total=511, active=106
    // curr: user=90, nice=15, sys=40, idle=425, iowait=8, irq=4, softirq=3, steal=2 -> total=587, active=154
    // delta_total = 587 - 511 = 76
    // delta_active = 154 - 106 = 48
    // usage % = (48 / 76) * 100 = 63.1578947%
    const double c0_util = telemetry::compute_cpu_utilization(state.m_previous[1], state.m_current[1]);
    assert(std::abs(c0_util - (48.0 / 76.0 * 100.0)) < 0.0001);

    std::cout << "  [PASS] test_cpu_synthetic_parsing exact arithmetic verified!" << std::endl;
}

static void test_cpu_collector_basic() {
    std::cout << "[RUN] test_cpu_collector_basic..." << std::endl;

    int recorded_fd = -1;
    {
        telemetry::CpuCollector collector;
        recorded_fd = collector.fd();
        assert(recorded_fd >= 0);
        // fcntl(fd, F_GETFD) asks the kernel to retrieve the file descriptor flags.
        assert(::fcntl(recorded_fd, F_GETFD) != -1); // Valid open descriptor

        bool ok = collector.sample();
        assert(ok);

        const auto& snap = collector.snapshot();
        // Aggregate CPU (index 0) must have non-zero jiffies on any running Linux machine
        assert(snap.m_current[0].total() > 0);
        assert(snap.detected_cores > 0);

        std::cout << "  [INFO] Detected CPU cores: " << snap.detected_cores << std::endl;
    }

    // Verify zero unclosed file descriptors: descriptor must be closed cleanly upon destruction!
    assert(::fcntl(recorded_fd, F_GETFD) == -1);
    // EBADF => Bad file descriptor
    assert(errno == EBADF);

    std::cout << "  [PASS] test_cpu_collector_basic verified (descriptor closed cleanly)!" << std::endl;
}

static void test_cpu_collector_zero_alloc_1000_iterations() {
    std::cout << "[RUN] test_cpu_collector_zero_alloc_1000_iterations..." << std::endl;

    telemetry::CpuCollector collector;
    // Initial sample to populate baseline
    bool ok = collector.sample();
    assert(ok);

    // Strict zero-allocation surveillance across 1,000 consecutive sampling ticks
    {
        AllocGuard guard;

        for (int i = 0; i < 1000; ++i) {
            ok = collector.sample();
            assert(ok);

            const auto& snap = collector.snapshot();
            const double util = telemetry::compute_cpu_utilization(snap.m_previous[0], snap.m_current[0]);
            assert(util >= 0.0 && util <= 100.0);
        }

        // Mechanically verify that strictly ZERO heap allocations occurred across all 1,000 iterations
        guard.assert_zero_allocations();
    }

    std::cout << "  [PASS] test_cpu_collector_zero_alloc_1000_iterations passed with 0 allocations!" << std::endl;
}

// 3. Core Scaling & Edge Conditions
// Test handling of systems with more cores than MAX_CORES (safe bounds truncation without buffer overflows)
static void test_cpu_more_cores_than_max() {
    std::cout << "[RUN] test_cpu_more_cores_than_max..." << std::endl;

    // Synthetic /proc/stat containing core IDs: 0, 63 (valid <= MAX_CORES), and 64, 65, 128 (beyond MAX_CORES)
    constexpr std::string_view mock_overflow_stat =
        "cpu  100 20 50 800 10 5 3 2 0 0\n"
        "cpu0 10 2 5 80 1 0 0 0 0 0\n"
        "cpu63 20 4 10 160 2 0 0 0 0 0\n"
        "cpu64 30 6 15 240 3 0 0 0 0 0\n"   // core_id == 64 >= MAX_CORES
        "cpu65 40 8 20 320 4 0 0 0 0 0\n"   // core_id == 65 >= MAX_CORES
        "cpu128 50 10 25 400 5 0 0 0 0 0\n" // core_id == 128 >= MAX_CORES
        "intr 112233\n";

    telemetry::CpuCollectorState state{};
    bool ok = telemetry::parse_proc_stat(mock_overflow_stat, state);
    assert(ok);

    // Aggregate core (index 0) parsed correctly
    assert(state.m_current[0].user == 100);

    // Core 0 (index 1) parsed correctly
    assert(state.m_current[1].user == 10);

    // Core 63 (index 64) parsed correctly
    assert(state.m_current[64].user == 20);

    // Cores >= MAX_CORES (64, 65, 128) must be truncated safely without buffer overflow
    assert(state.detected_cores == telemetry::MAX_CORES); // slot_index 64

    std::cout << "  [PASS] test_cpu_more_cores_than_max verified! Safe bounds truncation confirmed." << std::endl;
}

// Test zero-delta calculation: running consecutive sample() calls immediately should yield 0.0%
// without division-by-zero crashes, NaN, or Inf.
static void test_cpu_zero_delta_edge_case() {
    std::cout << "[RUN] test_cpu_zero_delta_edge_case..." << std::endl;

    telemetry::CpuJiffies jiffies{};
    jiffies.user = 500;
    jiffies.system = 200;
    jiffies.idle = 3000;

    // Case A: delta_total == 0 (both snapshots identical)
    double util = telemetry::compute_cpu_utilization(jiffies, jiffies);
    assert(util == 0.0);
    // Check 'Not a number'
    assert(!std::isnan(util));
    // Check 'Infinite'
    assert(!std::isinf(util));

    // Case B: wrap-around / non-monotonic edge case (curr < prev)
    telemetry::CpuJiffies wrapped_curr{};
    wrapped_curr.user = 10;
    wrapped_curr.idle = 50;
    util = telemetry::compute_cpu_utilization(jiffies, wrapped_curr);
    assert(util == 0.0);

    // Case C: Real collector back-to-back zero-delay sampling
    telemetry::CpuCollector collector;
    collector.sample();
    // Immediate subsequent sample with 0 delay
    collector.sample();
    const auto& snap = collector.snapshot();
    const double live_util = telemetry::compute_cpu_utilization(snap.m_previous[0], snap.m_current[0]);
    assert(live_util >= 0.0 && live_util <= 100.0);
    assert(!std::isnan(live_util));

    std::cout << "  [PASS] test_cpu_zero_delta_edge_case verified! 0.0% without division-by-zero." << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Running test_cpu harness..." << std::endl;
    std::cout << "========================================" << std::endl;

    test_cpu_synthetic_parsing();
    test_cpu_collector_basic();
    test_cpu_collector_zero_alloc_1000_iterations();
    test_cpu_more_cores_than_max();
    test_cpu_zero_delta_edge_case();

    std::cout << "========================================" << std::endl;
    std::cout << "All test_cpu checks passed successfully!" << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
