#pragma once

#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string_view>
#include <telemetry/cpu_collector.hpp>
#include <telemetry/mem_collector.hpp>
#include <telemetry/tcp_collector.hpp>
#include <thread>
#include <type_traits>

namespace telemetry {

// ==============================================================================
// 1. Composite Telemetry Snapshot Data Model (SystemSnapshot)
// ==============================================================================

// Flat, contiguous, trivially copyable struct containing all host metrics.
// 64-byte cache-line aligned (alignas(64)) to prevent false sharing across
// reader and writer CPU cores.
struct alignas(64) SystemSnapshot {
	// Header Metadata
	uint64_t timestamp_ns{0};			 // Monotonic timestamp of the collection tick
	uint64_t collection_duration_ns{0};	 // Time spent inside kernel /proc parsers
	uint64_t sequence_number{0};		 // Monotonically increasing generation counter

	// Subsystem Payloads
	CpuSnapshot cpu{};	// CPU aggregate & per-core jiffies and % utilizations
	MemSnapshot mem{};	// Memory & swap pressure metrics (KiB)
	TcpSnapshot tcp{};	// Dual-stack TCP state histograms & queue pressure
};

// Static compile-time verification of layout and concurrency constraints
static_assert(std::is_trivially_copyable_v<SystemSnapshot>, "SystemSnapshot must be trivially copyable for lock-free bitwise copies!");
static_assert(alignof(SystemSnapshot) == 64, "SystemSnapshot must be 64-byte cache-line aligned!");

// ==============================================================================
// 2. Non-Allocating Error Logging Primitive
// ==============================================================================

// POSIX direct write to STDERR_FILENO with zero dynamic memory allocation.
inline void log_error_raw(std::string_view msg) noexcept {
	if(!msg.empty()) {
		const ssize_t bytes_written = ::write(STDERR_FILENO, msg.data(), msg.size());
		// Silence the error of unused variable!
		(void)bytes_written;
	}
}

// ==============================================================================
// 3. Double-Buffered State Machine Architecture (SnapshotRegistry)
// ==============================================================================

// Two-slot lock-free snapshot registry coordinating between the single sampling
// writer thread and multiple concurrent reader threads (e.g. HTTP exporters).
class SnapshotRegistry {
   private:
	// Buffer Array: Two contiguous slots aligned to 64 bytes
	alignas(64) std::array<SystemSnapshot, 2> m_buffers{};

	// Active Index: Atomic integer with independent cache-line alignment
	alignas(64) std::atomic<uint32_t> m_active_index{0};

	// Active reader counts per buffer slot to guarantee zero data races under preemptive scheduling
	alignas(64) mutable std::array<std::atomic<uint32_t>, 2> m_reading_count{{{0}, {0}}};

	// Monotonically increasing 64-bit sequence counter (writer-exclusive)
	uint64_t m_sequence_counter{1};

   public:
	SnapshotRegistry() noexcept = default;

	// Non-copyable and non-movable to anchor concurrent buffer addresses in memory
	SnapshotRegistry(const SnapshotRegistry&) = delete;
	SnapshotRegistry& operator=(const SnapshotRegistry&) = delete;
	SnapshotRegistry(SnapshotRegistry&&) = delete;
	SnapshotRegistry& operator=(SnapshotRegistry&&) = delete;

	// Read Path Protocol (Prometheus / Scraper Threads):
	// Loads the current active index using load-acquire and returns a direct const reference
	// to the active, immutable snapshot. Lock-free O(1) read access without write contention.
	const SystemSnapshot& get_active_snapshot() const noexcept {
		// acquire + load -> Reader
		const uint32_t front_idx = m_active_index.load(std::memory_order_acquire);
		return m_buffers[front_idx];
	}

	// Copies the active snapshot onto the caller's stack frame via trivial bitwise copy
	// with generation tracking to ensure 100% race-free and tear-free read isolation.
	SystemSnapshot copy_active_snapshot() const noexcept {
		while(true) {
			const uint32_t front_idx = m_active_index.load(std::memory_order_acquire);
			m_reading_count[front_idx].fetch_add(1, std::memory_order_acquire);

			// Verify the active buffer did not flip while incrementing reader count
			if(m_active_index.load(std::memory_order_acquire) != front_idx) {
				m_reading_count[front_idx].fetch_sub(1, std::memory_order_release);
				continue;
			}

			const SystemSnapshot copy = m_buffers[front_idx];
			m_reading_count[front_idx].fetch_sub(1, std::memory_order_release);
			return copy;
		}
	}

	// Returns current active index for diagnostic/testing purposes
	uint32_t active_index() const noexcept {
		// Standalone counters -> relaxed
		return m_active_index.load(std::memory_order_relaxed);
	}

	// Write Path Protocol (Sampling Thread):
	// 1. Identifies the inactive back-buffer slot: back_idx = 1 - active_idx.
	// 2. Waits for any trailing readers on back_idx to finish copying.
	// 3. Samples directly into m_buffers[back_idx] with error containment.
	// 4. Updates timestamp, duration, and increments sequence number.
	// 5. Atomically publishes the new snapshot using store-release.
	void sample_and_publish(CpuCollector& cpu_col, MemCollector& mem_col, TcpCollector& tcp_col) noexcept {
		// Access the curent active index!
		const uint32_t current_active = m_active_index.load(std::memory_order_relaxed);
		// We will write in the other one (the non-active one)
		const uint32_t back_idx = 1 - current_active;

		// Ensure any slow/preempted reader on back_idx has completed before writer re-uses slot
		while(m_reading_count[back_idx].load(std::memory_order_acquire) != 0) {
			std::this_thread::yield();
		}

		auto& target = m_buffers[back_idx];
		const auto& active = m_buffers[current_active];

		// Monotonic clock!
		const auto t_start = std::chrono::steady_clock::now();

		// 1. Sample CPU with failure containment: fall back to last known good metrics on error
		if(cpu_col.sample()) {
			target.cpu = cpu_col.snapshot();
		} else {
			log_error_raw("[telemetryd] Warning: CPU sample failed, preserving last known metrics.\n");
			target.cpu = active.cpu;
		}

		// 2. Sample Memory with failure containment
		if(mem_col.sample()) {
			target.mem = mem_col.snapshot();
		} else {
			log_error_raw("[telemetryd] Warning: Memory sample failed, preserving last known metrics.\n");
			target.mem = active.mem;
		}

		// 3. Sample TCP with failure containment
		if(tcp_col.sample()) {
			target.tcp = tcp_col.snapshot();
		} else {
			log_error_raw("[telemetryd] Warning: TCP sample failed, preserving last known metrics.\n");
			target.tcp = active.tcp;
		}

		const auto t_end = std::chrono::steady_clock::now();

		// Update buffer metadata
		// time_since_epoch() -> Time since the moment Linux machine booted up
		target.timestamp_ns = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(t_end.time_since_epoch()).count());
		target.collection_duration_ns = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count());
		target.sequence_number = m_sequence_counter++;

		// Publish new snapshot using store-release barrier
		// Activate the non-active one
		m_active_index.store(back_idx, std::memory_order_release);
	}

	// Direct publish method for testing with synthetic snapshots
	void publish(const SystemSnapshot& snapshot) noexcept {
		const uint32_t current_active = m_active_index.load(std::memory_order_relaxed);
		const uint32_t back_idx = 1 - current_active;

		while(m_reading_count[back_idx].load(std::memory_order_acquire) != 0) {
			std::this_thread::yield();
		}

		m_buffers[back_idx] = snapshot;
		m_buffers[back_idx].sequence_number = m_sequence_counter++;

		m_active_index.store(back_idx, std::memory_order_release);
	}
};

// ==============================================================================
// 4. The Sampling Engine Thread (SamplingEngine)
// ==============================================================================

// Driver managing the collection cadence via absolute steady_clock deadlines
// and clean signal/lifecycle termination.
class SamplingEngine {
   private:
	SnapshotRegistry& m_registry;
	std::chrono::milliseconds m_interval;
	std::atomic<bool> m_running{false};
	std::thread m_thread{};

	// Persistent collectors owned by the engine
	CpuCollector m_cpu_col{};
	MemCollector m_mem_col{};
	TcpCollector m_tcp_col{};

	void thread_loop() {
		auto next_tick = std::chrono::steady_clock::now();

		while(m_running.load(std::memory_order_relaxed)) {
			// Absolute deadline: eliminates cumulative clock drift
			next_tick += m_interval;

			// Perform zero-allocation sample & publish across all collectors
			m_registry.sample_and_publish(m_cpu_col, m_mem_col, m_tcp_col);

			// Sleep until the exact absolute time point
			std::this_thread::sleep_until(next_tick);
		}
	}

   public:
	explicit SamplingEngine(SnapshotRegistry& registry, std::chrono::milliseconds interval = std::chrono::milliseconds(1000))
		: m_registry(registry), m_interval(interval) {}

	~SamplingEngine() {
		stop();
	}

	// Non-copyable and non-movable
	SamplingEngine(const SamplingEngine&) = delete;
	SamplingEngine& operator=(const SamplingEngine&) = delete;
	SamplingEngine(SamplingEngine&&) = delete;
	SamplingEngine& operator=(SamplingEngine&&) = delete;

	// Start background sampling thread
	void start() {
		// exchange -> atomically replace the value of m_running by 'true' and return the old value
		if(m_running.exchange(true, std::memory_order_acq_rel)) {
			return;	 // Already running
		}
		// "Hey OS, hire a new worker (a thread) and have them run the function thread_loop
		// on this specific SamplingEngine object right now in the background."
		m_thread = std::thread(&SamplingEngine::thread_loop, this);
	}

	// Clean, leak-free stop mechanism
	void stop() noexcept {
		if(!m_running.exchange(false, std::memory_order_acq_rel)) {
			return;	 // Already stopped
		}
		if(m_thread.joinable()) {
			m_thread.join();
		}
	}

	bool is_running() const noexcept {
		return m_running.load(std::memory_order_relaxed);
	}

	std::chrono::milliseconds interval() const noexcept {
		return m_interval;
	}
};

}  // namespace telemetry
