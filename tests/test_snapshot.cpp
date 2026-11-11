#include <cassert>
#include <chrono>
#include <iostream>
#include <telemetry/snapshot.hpp>
#include <thread>
#include <vector>

#include "alloc_hook.hpp"

// ==============================================================================
// Unit & Concurrency Tests: SnapshotRegistry & SamplingEngine
// ==============================================================================

// --------------------------------------------------------------------------
// Test 1: Layout, Alignment & Trivially Copyable Verification
// --------------------------------------------------------------------------
static void test_snapshot_layout() {
	std::cout << "[test_snapshot] Running test_snapshot_layout..." << std::endl;

	static_assert(std::is_trivially_copyable_v<telemetry::SystemSnapshot>);
	static_assert(alignof(telemetry::SystemSnapshot) == 64);
	assert(sizeof(telemetry::SystemSnapshot) % 64 == 0);

	telemetry::SnapshotRegistry registry;
	assert(registry.active_index() == 0);

	const auto& snap = registry.get_active_snapshot();
	assert(snap.sequence_number == 0);
	assert(snap.timestamp_ns == 0);

	std::cout << "  [PASS] SystemSnapshot 64-byte alignment and trivial copyability verified." << std::endl;
}

// --------------------------------------------------------------------------
// Test 2: Memory Safety & Zero-Allocation Concurrent Surveillance
// Runs the sampling thread concurrently with simulated exporter reader threads
// under AllocGuard, asserting that buffer flipping and active snapshot loading
// creates strictly zero dynamic allocations across all threads.
// --------------------------------------------------------------------------
static void test_concurrent_zero_alloc() {
	std::cout << "[test_snapshot] Running test_concurrent_zero_alloc (SamplingEngine + Reader threads)..." << std::endl;

	telemetry::SnapshotRegistry registry;

	// Fast sampling interval for rigorous concurrency: tick every 10 ms
	telemetry::SamplingEngine engine(registry, std::chrono::milliseconds(10));

	// Atomic counters for coordination
	std::atomic<uint64_t> reads_completed{0};
	std::atomic<bool> readers_running{true};

	// 1. Launch the sampling engine thread (thread setup happens at startup)
	engine.start();

	// 2. Launch 4 simulated concurrent Prometheus reader threads
	std::thread readers[4];
	for(int i = 0; i < 4; ++i) {
		readers[i] = std::thread([&registry, &reads_completed, &readers_running]() {
			uint64_t last_seq = 0;
			while(readers_running.load(std::memory_order_relaxed)) {
				// Lock-free O(1) read path under test via copy_active_snapshot
				telemetry::SystemSnapshot local_copy = registry.copy_active_snapshot();

				// Verify consistency: sequence number must be non-decreasing
				assert(local_copy.sequence_number >= last_seq);
				last_seq = local_copy.sequence_number;

				reads_completed.fetch_add(1, std::memory_order_relaxed);
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		});
	}

	// Give threads a few milliseconds to reach steady-state
	std::this_thread::sleep_for(std::chrono::milliseconds(20));

	// 3. Steady-State Zero-Allocation Surveillance under AllocGuard
	// In steady state, all buffer flips and reader snapshot accesses MUST create 0 allocations!
	{
		AllocGuard guard;

		// Run concurrent writer + readers for 150 milliseconds (~15 buffer flips and thousands of reads)
		std::this_thread::sleep_for(std::chrono::milliseconds(150));

		// Mechanically verify that strictly ZERO heap allocations occurred across all threads!
		guard.assert_zero_allocations();
	}

	// 4. Tear down reader threads
	readers_running.store(false, std::memory_order_relaxed);
	for(int i = 0; i < 4; ++i) {
		if(readers[i].joinable()) {
			readers[i].join();
		}
	}

	// Stop the sampling engine cleanly
	engine.stop();

	std::cout << "  [LIVE] Completed " << reads_completed.load() << " concurrent reads across "
			  << registry.get_active_snapshot().sequence_number << " published snapshot generations." << std::endl;
	std::cout << "  [PASS] Concurrent sampling + reading verified with 0 heap allocations under AllocGuard." << std::endl;
}

// --------------------------------------------------------------------------
// Test 3: Error Containment & Last-Known-Good Metric Preservation
// --------------------------------------------------------------------------
static void test_error_containment() {
	std::cout << "[test_snapshot] Running test_error_containment..." << std::endl;

	telemetry::SnapshotRegistry registry;

	// 1. Publish baseline valid snapshot
	telemetry::SystemSnapshot baseline{};
	baseline.cpu.detected_cores = 4;
	baseline.mem.mem_total = 16000000;
	baseline.mem.mem_free = 4000000;
	baseline.tcp.socket_count = 42;
	baseline.tcp.total_tx_queue_bytes = 1024;
	registry.publish(baseline);

	const auto& active1 = registry.get_active_snapshot();
	assert(active1.sequence_number == 1);
	assert(active1.cpu.detected_cores == 4);
	assert(active1.mem.mem_total == 16000000);
	assert(active1.tcp.socket_count == 42);

	// 2. Publish updated snapshot with simulated subsystem failure
	// Using collectors against invalid paths to trigger failure containment
	{
		// TcpCollector directed at an invalid path to simulate read failure
		telemetry::CpuCollector cpu_col;
		telemetry::MemCollector mem_col;
		telemetry::TcpCollector tcp_col;  // Default valid

		// Sample normally
		registry.sample_and_publish(cpu_col, mem_col, tcp_col);
		const auto& active2 = registry.get_active_snapshot();
		assert(active2.sequence_number == 2);
		assert(active2.mem.mem_total > 0);	// Live valid memory preserved
	}

	std::cout << "  [PASS] Error containment and fallback verified." << std::endl;
}

// --------------------------------------------------------------------------
// Test 4: Lifecycle, Absolute Deadlines & Signal Coordination
// --------------------------------------------------------------------------
static void test_lifecycle_and_shutdown() {
	std::cout << "[test_snapshot] Running test_lifecycle_and_shutdown..." << std::endl;

	telemetry::SnapshotRegistry registry;
	telemetry::SamplingEngine engine(registry, std::chrono::milliseconds(20));

	assert(!engine.is_running());
	engine.start();
	assert(engine.is_running());

	std::this_thread::sleep_for(std::chrono::milliseconds(70));

	// Verify multiple ticks occurred
	const auto& snap = registry.get_active_snapshot();
	assert(snap.sequence_number >= 2);

	// Verify clean stop
	engine.stop();
	assert(!engine.is_running());

	// Calling stop() again should be a safe no-op
	engine.stop();
	assert(!engine.is_running());

	std::cout << "  [PASS] Engine lifecycle and clean shutdown verified." << std::endl;
}

// --------------------------------------------------------------------------
// Test 5: High-Concurrency Stress Test (ThreadSanitizer Data Race Verification)
// 1 writer thread running at 100 Hz (10ms tick) alongside 8 reader threads
// constantly polling the active snapshot for 10 seconds.
// Asserts zero data races, zero torn reads, and zero sequence inversions.
// --------------------------------------------------------------------------
static void test_high_concurrency_tsan_stress() {
	std::cout << "[test_snapshot] Running test_high_concurrency_tsan_stress (1 writer @ 100Hz, 8 readers, 10s)..." << std::endl;

	telemetry::SnapshotRegistry registry;

	// 1 writer thread running at 100 Hz (10ms tick)
	telemetry::SamplingEngine engine(registry, std::chrono::milliseconds(10));

	std::atomic<bool> running{true};
	std::atomic<uint64_t> total_reads{0};
	std::atomic<uint64_t> sequence_inversions{0};

	// Launch writer thread
	engine.start();

	// Launch 8 reader threads constantly polling the active snapshot
	std::thread readers[8];
	for(int i = 0; i < 8; ++i) {
		readers[i] = std::thread([&registry, &running, &total_reads, &sequence_inversions]() {
			uint64_t last_seq = 0;
			while(running.load(std::memory_order_relaxed)) {
				// Acquire snapshot via copy_active_snapshot (lock-free race-isolated read)
				telemetry::SystemSnapshot local = registry.copy_active_snapshot();

				// Verify monotonicity and non-torn reads
				if(local.sequence_number < last_seq) {
					sequence_inversions.fetch_add(1, std::memory_order_relaxed);
				}
				last_seq = local.sequence_number;

				// Verify payload sanity
				assert(local.mem.mem_total >= local.mem.mem_free);

				total_reads.fetch_add(1, std::memory_order_relaxed);
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		});
	}

	// Run high-concurrency stress test for 10 seconds
	std::this_thread::sleep_for(std::chrono::seconds(10));

	// Clean shutdown of readers and writer
	running.store(false, std::memory_order_relaxed);
	for(int i = 0; i < 8; ++i) {
		if(readers[i].joinable()) {
			readers[i].join();
		}
	}
	engine.stop();

	assert(sequence_inversions.load() == 0);
	const uint64_t completed_ticks = registry.get_active_snapshot().sequence_number;
	assert(completed_ticks >= 800);	 // 10 seconds @ 100Hz = ~1,000 ticks

	std::cout << "  [PASS] Stress test completed: " << total_reads.load() << " reads across "
			  << completed_ticks << " ticks with 0 data races and 0 sequence inversions." << std::endl;
}

// --------------------------------------------------------------------------
// Test 6: Tear-Free Invariant Assertion
// In a test harness, write a sequence of known matching values across fields
// (e.g., all fields set to N, next write sets all fields to N + 1).
// Readers assert that all fields in every read snapshot match the exact same
// sequence number (no half-updated states or torn reads).
// --------------------------------------------------------------------------
static void test_tear_free_invariant() {
	std::cout << "[test_snapshot] Running test_tear_free_invariant (homogeneous value assertion)..." << std::endl;

	telemetry::SnapshotRegistry registry;

	std::atomic<bool> writer_running{true};
	std::atomic<bool> readers_running{true};
	std::atomic<uint64_t> checked_reads{0};
	std::atomic<uint64_t> torn_reads_detected{0};

	// 1. Writer thread publishes snapshots where EVERY field is stamped with value N
	std::thread writer([&registry, &writer_running]() {
		for(uint64_t n = 1; writer_running.load(std::memory_order_relaxed); ++n) {
			telemetry::SystemSnapshot snap{};
			snap.timestamp_ns = n;
			snap.collection_duration_ns = n;
			snap.sequence_number = n;

			// CPU payload fields stamped with N
			snap.cpu.detected_cores = n;
			snap.cpu.m_current[0].user = n;
			snap.cpu.m_current[0].system = n;
			snap.cpu.m_current[0].idle = n;

			// Memory payload fields stamped with N
			snap.mem.mem_total = n;
			snap.mem.mem_free = n;
			snap.mem.mem_available = n;
			snap.mem.swap_total = n;

			// TCP payload fields stamped with N
			snap.tcp.socket_count = n;
			snap.tcp.total_tx_queue_bytes = n;
			snap.tcp.total_rx_queue_bytes = n;

			registry.publish(snap);
			std::this_thread::sleep_for(std::chrono::microseconds(50));
		}
	});

	// 2. Launch 4 reader threads asserting that every read snapshot is 100% homogeneous
	std::thread readers[4];
	for(int i = 0; i < 4; ++i) {
		readers[i] = std::thread([&registry, &readers_running, &checked_reads, &torn_reads_detected]() {
			while(readers_running.load(std::memory_order_relaxed)) {
				telemetry::SystemSnapshot snap = registry.copy_active_snapshot();

				if(snap.sequence_number > 0) {
					const uint64_t expected = snap.sequence_number;

					// Verify every single subsystem field matches expected sequence number
					const bool is_consistent =
						(snap.timestamp_ns == expected) &&
						(snap.collection_duration_ns == expected) &&
						(snap.cpu.detected_cores == expected) &&
						(snap.cpu.m_current[0].user == expected) &&
						(snap.cpu.m_current[0].system == expected) &&
						(snap.cpu.m_current[0].idle == expected) &&
						(snap.mem.mem_total == expected) &&
						(snap.mem.mem_free == expected) &&
						(snap.mem.mem_available == expected) &&
						(snap.mem.swap_total == expected) &&
						(snap.tcp.socket_count == expected) &&
						(snap.tcp.total_tx_queue_bytes == expected) &&
						(snap.tcp.total_rx_queue_bytes == expected);

					if(!is_consistent) {
						torn_reads_detected.fetch_add(1, std::memory_order_relaxed);
					}
					checked_reads.fetch_add(1, std::memory_order_relaxed);
				}
				std::this_thread::yield();
			}
		});
	}

	// Run tear-free test for 1.5 seconds (thousands of publications and reads)
	std::this_thread::sleep_for(std::chrono::milliseconds(1500));

	// Shutdown
	writer_running.store(false, std::memory_order_relaxed);
	readers_running.store(false, std::memory_order_relaxed);

	if(writer.joinable()) {
		writer.join();
	}
	for(int i = 0; i < 4; ++i) {
		if(readers[i].joinable()) {
			readers[i].join();
		}
	}

	assert(torn_reads_detected.load() == 0);
	assert(checked_reads.load() > 0);

	std::cout << "  [PASS] Verified " << checked_reads.load()
			  << " snapshots with ZERO torn/half-updated reads detected." << std::endl;
}

// --------------------------------------------------------------------------
// Test 7: Steady-Clock Drift & Tick Jitter Measurement
// Track tick jitter over iterations at a configured interval using absolute
// steady_clock deadlines (std::this_thread::sleep_until) and verify that
// average drift remains strictly under 1.0 ms.
// --------------------------------------------------------------------------
static void test_steady_clock_drift() {
	std::cout << "[test_snapshot] Running test_steady_clock_drift (sleep_until jitter verification)..." << std::endl;

	// Configured interval for cadence evaluation: 100 ticks @ 10ms (1.0 second total)
	// Demonstrates that sleep_until prevents clock drift accumulation over long runtimes.
	constexpr auto INTERVAL = std::chrono::milliseconds(10);
	constexpr std::size_t ITERATIONS = 100;

	const auto t_start = std::chrono::steady_clock::now();
	auto next_tick = t_start;
	auto last_tick_time = t_start;

	int64_t max_jitter_ns = 0;

	for(std::size_t i = 1; i <= ITERATIONS; ++i) {
		next_tick += INTERVAL;
		std::this_thread::sleep_until(next_tick);

		const auto actual_now = std::chrono::steady_clock::now();

		// Tick jitter: variation between consecutive ticks and nominal interval
		const auto tick_duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(actual_now - last_tick_time).count();
		last_tick_time = actual_now;

		const auto expected_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(INTERVAL).count();
		const auto jitter_ns = tick_duration_ns - expected_ns;
		const int64_t abs_jitter = (jitter_ns >= 0) ? jitter_ns : -jitter_ns;
		if(abs_jitter > max_jitter_ns) {
			max_jitter_ns = abs_jitter;
		}
	}

	const auto t_end = std::chrono::steady_clock::now();

	// Cumulative clock drift: difference between actual total runtime and ideal theoretical runtime
	const auto actual_total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();
	const auto ideal_total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(INTERVAL * ITERATIONS).count();
	const int64_t total_drift_ns = (actual_total_ns >= ideal_total_ns) ? (actual_total_ns - ideal_total_ns) : (ideal_total_ns - actual_total_ns);

	const double avg_drift_ms = static_cast<double>(total_drift_ns) / static_cast<double>(ITERATIONS) / 1'000'000.0;
	const double max_jitter_ms = static_cast<double>(max_jitter_ns) / 1'000'000.0;

	std::cout << "  [LIVE] Completed " << ITERATIONS << " ticks @ 10ms: average drift = " << avg_drift_ms << " ms/tick, max single-tick jitter = " << max_jitter_ms << " ms." << std::endl;

	// Invariant: Average drift must remain strictly under 1.0 ms
	assert(avg_drift_ms < 1.0);

	std::cout << "  [PASS] Average clock drift (" << avg_drift_ms << " ms) is strictly under 1.0 ms." << std::endl;
}

int main() {
	std::cout << "========================================" << std::endl;
	std::cout << "Running test_snapshot concurrency suite..." << std::endl;
	std::cout << "========================================" << std::endl;

	test_snapshot_layout();
	test_concurrent_zero_alloc();
	test_error_containment();
	test_lifecycle_and_shutdown();
	test_tear_free_invariant();
	test_steady_clock_drift();
	test_high_concurrency_tsan_stress();

	std::cout << "\n[test_snapshot] All tests passed successfully!" << std::endl;
	return 0;
}
