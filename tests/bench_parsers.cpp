#include <chrono>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <telemetry/cpu_collector.hpp>
#include <telemetry/mem_collector.hpp>
#include <telemetry/tcp_collector.hpp>

// Helper to prevent compiler from optimizing away benchmark operations
// This is GCC/Clang inline assembly:-
// It tricks the optimizing compiler into believing the benchmark result is actively used, ensuring your CPU actually runs the parser code on every iteration
// without adding artificial latency.
static void do_not_optimize(const void* ptr) {
	asm volatile("" : : "r,m"(ptr) : "memory");
}

// Because The System Had 0 TCP Sockets!
// Representative /proc/net/tcp buffer with 10 socket rows
static constexpr std::string_view BENCH_TCP_BUFFER =
	"  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
	"   0: 00000000:0050 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 12345 1 0000000000000000 100 0 0 10 0\n"
	"   1: 0100007F:1F90 0100007F:C350 01 00000020:00000040 00:00000000 00000000  1000        0 67890 1 0000000000000000 100 0 0 10 0\n"
	"   2: 0100007F:C350 0100007F:1F90 06 00000000:00000000 00:00000000 00000000  1000        0 67891 1 0000000000000000 100 0 0 10 0\n"
	"   3: 00000000:0016 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 10001 1 0000000000000000 100 0 0 10 0\n"
	"   4: 0100007F:0457 0100007F:0458 01 00000010:00000020 00:00000000 00000000  1000        0 10002 1 0000000000000000 100 0 0 10 0\n"
	"   5: 0100007F:0458 0100007F:0457 01 00000000:00000000 00:00000000 00000000  1000        0 10003 1 0000000000000000 100 0 0 10 0\n"
	"   6: 00000000:238C 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 10004 1 0000000000000000 100 0 0 10 0\n"
	"   7: 0100007F:238C 0100007F:D4A2 01 00000100:00000200 00:00000000 00000000  1000        0 10005 1 0000000000000000 100 0 0 10 0\n"
	"   8: 0100007F:D4A2 0100007F:238C 01 00000000:00000000 00:00000000 00000000  1000        0 10006 1 0000000000000000 100 0 0 10 0\n"
	"   9: 0100007F:E510 0100007F:0050 06 00000000:00000000 00:00000000 00000000  1000        0 10007 1 0000000000000000 100 0 0 10 0\n";

int main() {
	std::cout << "=================================================================" << std::endl;
	std::cout << "  telemetryd: High-Precision Parser Microbenchmark (Release)" << std::endl;
	std::cout << "=================================================================" << std::endl;

	telemetry::CpuCollector cpu_collector;
	telemetry::MemCollector mem_collector;

	// The warm-up runs discard the one-time startup "tax" so we measure the true steady-state runtime performance of the daemon.
	// Warmup runs
	for(int i = 0; i < 1000; ++i) {
		cpu_collector.collect();
		mem_collector.collect();
	}

	constexpr int ITERATIONS = 50000;

	// 1. Benchmark /proc/stat parser (aggregate + per-core jiffies)
	auto start_cpu = std::chrono::steady_clock::now();
	for(int i = 0; i < ITERATIONS; ++i) {
		cpu_collector.collect();
		const auto& s = cpu_collector.snapshot();
		do_not_optimize(&s);
	}
	auto end_cpu = std::chrono::steady_clock::now();
	const double cpu_ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end_cpu - start_cpu).count()) / ITERATIONS;

	// 2. Benchmark /proc/meminfo parser
	auto start_mem = std::chrono::steady_clock::now();
	for(int i = 0; i < ITERATIONS; ++i) {
		mem_collector.collect();
		const auto& s = mem_collector.snapshot();
		do_not_optimize(&s);
	}
	auto end_mem = std::chrono::steady_clock::now();
	const double mem_ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end_mem - start_mem).count()) / ITERATIONS;

	// 3. Benchmark pure in-memory /proc/net/tcp parser (decoding 10 socket rows)
	telemetry::TcpSnapshot tcp_snap{};
	auto start_tcp = std::chrono::steady_clock::now();
	for(int i = 0; i < ITERATIONS; ++i) {
		tcp_snap.reset();
		telemetry::parse_tcp_buffer(BENCH_TCP_BUFFER, tcp_snap);
		do_not_optimize(&tcp_snap);
	}
	auto end_tcp = std::chrono::steady_clock::now();
	const double tcp_batch_ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end_tcp - start_tcp).count()) / ITERATIONS;

	constexpr double SOCKET_ROWS = 10.0;
	const double tcp_ns_per_row = tcp_batch_ns / SOCKET_ROWS;

	// 4. Combined collection tick overhead (CPU parser + Mem parser + TCP decoding)
	const double total_collection_ns = cpu_ns + mem_ns + tcp_batch_ns;
	const double total_collection_us = total_collection_ns / 1000.0;
	// CPU % at 1 Hz = total_collection_ns / 1,000,000,000 ns * 100%
	const double cpu_footprint_1hz = (total_collection_ns / 1e9) * 100.0;

	std::cout << std::fixed << std::setprecision(2);
	std::cout << "\nResults across " << ITERATIONS << " parser passes:" << std::endl;
	std::cout << "-----------------------------------------------------------------" << std::endl;
	std::cout << "  1. /proc/stat (aggregate + " << cpu_collector.snapshot().detected_cores << " cores):  "
			  << std::setw(8) << cpu_ns << " ns  (" << (cpu_ns / 1000.0) << " us)" << std::endl;
	std::cout << "  2. /proc/meminfo:                      "
			  << std::setw(8) << mem_ns << " ns  (" << (mem_ns / 1000.0) << " us)" << std::endl;
	std::cout << "  3. /proc/net/tcp (10 socket rows decoded): "
			  << std::setw(8) << tcp_batch_ns << " ns  (" << (tcp_batch_ns / 1000.0) << " us)" << std::endl;
	std::cout << "     └─ Exact cost per socket row:       "
			  << std::setw(8) << tcp_ns_per_row << " ns/row" << std::endl;
	std::cout << "-----------------------------------------------------------------" << std::endl;
	std::cout << "  TOTAL COMBINED TICK OVERHEAD:          "
			  << std::setw(8) << total_collection_ns << " ns  (" << total_collection_us << " us)" << std::endl;
	std::cout << "  ESTIMATED CPU FOOTPRINT AT 1 Hz:        "
			  << std::setprecision(5) << cpu_footprint_1hz << " % CPU (< 0.1% target)" << std::endl;
	std::cout << "=================================================================\n"
			  << std::endl;

	return 0;
}
