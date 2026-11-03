#include <fcntl.h>
#include <unistd.h>

#include <cassert>
#include <cstring>
#include <iostream>
#include <string_view>
#include <telemetry/tcp_collector.hpp>

#include "alloc_hook.hpp"

// Synthetic mock output resembling /proc/net/tcp
static constexpr std::string_view MOCK_PROC_NET_TCP =
	"  sl  local_address rem_address   st tx_queue rx_queue tr tm->when "
	"retrnsmt   uid  timeout inode\n"
	"   0: 00000000:0050 00000000:0000 0A 00000000:00000000 00:00000000 "
	"00000000     0        0 12345 1 0000000000000000 100 0 0 10 0\n"
	"   1: 0100007F:1F90 0100007F:C350 01 00000020:00000040 00:00000000 "
	"00000000  1000        0 67890 1 0000000000000000 100 0 0 10 0\n"
	"   2: 0100007F:C350 0100007F:1F90 06 00000000:00000000 00:00000000 "
	"00000000  1000        0 67891 1 0000000000000000 100 0 0 10 0\n";

int main() {
	std::cout << "[test_tcp] Starting TCP collector tests..." << std::endl;

	// --------------------------------------------------------------------------
	// Test 1: Synthetic buffer parsing & metric aggregation
	// --------------------------------------------------------------------------
	{
		telemetry::TcpSnapshot snapshot{};
		telemetry::parse_tcp_buffer(MOCK_PROC_NET_TCP, snapshot);

		// Verify total sockets parsed (3 sockets, header skipped)
		assert(snapshot.socket_count == 3);

		// Verify TCP states:
		// Index 0x0A (LISTEN) -> 1 socket
		// Index 0x01 (ESTABLISHED) -> 1 socket
		// Index 0x06 (TIME_WAIT) -> 1 socket
		assert(snapshot.state_counts[static_cast<std::size_t>(telemetry::TcpState::LISTEN)] == 1);
		assert(snapshot.state_counts[static_cast<std::size_t>(telemetry::TcpState::ESTABLISHED)] == 1);
		assert(snapshot.state_counts[static_cast<std::size_t>(telemetry::TcpState::TIME_WAIT)] == 1);
		assert(snapshot.state_counts[static_cast<std::size_t>(telemetry::TcpState::SYN_SENT)] == 0);

		// Verify queue bytes:
		// Line 1: tx=0, rx=0
		// Line 2: tx=0x20 (32 bytes), rx=0x40 (64 bytes)
		// Line 3: tx=0, rx=0
		// Total tx = 32 bytes
		// Total rx = 64 bytes
		// Max tx = 32 bytes
		// Max rx = 64 bytes
		assert(snapshot.total_tx_queue_bytes == 32);
		assert(snapshot.total_rx_queue_bytes == 64);
		assert(snapshot.max_tx_queue_bytes == 32);
		assert(snapshot.max_rx_queue_bytes == 64);

		std::cout << "  [PASS] Synthetic /proc/net/tcp parsing verified." << std::endl;
	}

	// --------------------------------------------------------------------------
	// Test 2: Line Boundary Splicing simulation across chunks
	// --------------------------------------------------------------------------
	{
		// Split MOCK_PROC_NET_TCP right in the middle of line 2:
		// "... 01 00000020:00" and "000040 00:00000000 ... \n ..."
		const std::size_t split_pos = MOCK_PROC_NET_TCP.find("00000020:00") + 11;
		const std::string_view chunk1 = MOCK_PROC_NET_TCP.substr(0, split_pos);
		const std::string_view chunk2 = MOCK_PROC_NET_TCP.substr(split_pos);

		telemetry::TcpSnapshot spliced_snapshot{};
		char buffer[1024];
		std::size_t tail_len = 0;

		// Process chunk 1
		std::memcpy(buffer + tail_len, chunk1.data(), chunk1.size());
		std::size_t total_len1 = tail_len + chunk1.size();
		std::string_view view1(buffer, total_len1);
		auto last_nl1 = view1.rfind('\n');
		assert(last_nl1 != std::string_view::npos);

		// Parse up to last newline in chunk 1
		telemetry::parse_tcp_buffer(view1.substr(0, last_nl1 + 1),
									spliced_snapshot);

		// Shift tail to front
		tail_len = total_len1 - (last_nl1 + 1);
		std::memmove(buffer, buffer + last_nl1 + 1, tail_len);

		// Append chunk 2
		std::memcpy(buffer + tail_len, chunk2.data(), chunk2.size());
		std::size_t total_len2 = tail_len + chunk2.size();
		std::string_view view2(buffer, total_len2);
		auto last_nl2 = view2.rfind('\n');
		assert(last_nl2 != std::string_view::npos);

		// Parse remaining complete lines
		telemetry::parse_tcp_buffer(view2.substr(0, last_nl2 + 1),
									spliced_snapshot);

		// Verify that the spliced result matches the full unchunked parse
		assert(spliced_snapshot.socket_count == 3);
		assert(spliced_snapshot.total_tx_queue_bytes == 32);
		assert(spliced_snapshot.total_rx_queue_bytes == 64);
		assert(spliced_snapshot.max_tx_queue_bytes == 32);
		assert(spliced_snapshot.max_rx_queue_bytes == 64);

		std::cout << "  [PASS] Line boundary chunk splicing verified."
				  << std::endl;
	}

	// --------------------------------------------------------------------------
	// Test 2b: Multi-chunk 64 KiB buffer boundary straddling via TcpCollector
	// Generates a ~70 KiB virtual file to force TcpCollector's read() loop to
	// slice a socket row across its internal 64 KiB boundary and verify seamless splicing.
	// --------------------------------------------------------------------------
	{
		char temp_path[] = "/tmp/telemetryd_tcp_splice_XXXXXX";
		// The mkstemp function in C++ is used to create a unique temporary file
		// by replacing the last six characters of the provided template string
		// (which must end with "XXXXXX") with a unique combination of characters.
		// It returns a file descriptor for the newly created file, or -1 on failure.
		const int temp_fd = ::mkstemp(temp_path);
		assert(temp_fd >= 0);

		// 1. Write column header line
		const std::string_view header =
			"  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n";
		ssize_t w = ::write(temp_fd, header.data(), header.size());
		assert(w == static_cast<ssize_t>(header.size()));

		// 2. Write 600 socket lines (~125 bytes each -> ~75 KiB total, crossing 64 KiB boundary)
		// Line 1 contains 32 tx bytes and 64 rx bytes in ESTABLISHED state
		const std::string_view target_line =
			"   1: 0100007F:1F90 0100007F:C350 01 00000020:00000040 00:00000000 00000000  1000        0 67890 1 0000000000000000 100 0 0 10 0\n";
		w = ::write(temp_fd, target_line.data(), target_line.size());
		assert(w == static_cast<ssize_t>(target_line.size()));

		// Remaining 599 lines are dummy LISTEN sockets with 0 queue bytes
		const std::string_view dummy_line =
			"   0: 00000000:0050 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 12345 1 0000000000000000 100 0 0 10 0\n";
		for(int i = 0; i < 599; ++i) {
			w = ::write(temp_fd, dummy_line.data(), dummy_line.size());
			assert(w == static_cast<ssize_t>(dummy_line.size()));
		}
		::close(temp_fd);

		// 3. Sample through TcpCollector using the 75 KiB file
		{
			telemetry::TcpCollector collector(temp_path, "/dev/null");
			const bool ok = collector.sample();
			assert(ok);

			const auto& snap = collector.snapshot();
			// Exactly 600 sockets tracked (1 target + 599 dummy)
			assert(snap.socket_count == 600);
			assert(snap.state_counts[static_cast<std::size_t>(telemetry::TcpState::ESTABLISHED)] == 1);
			assert(snap.state_counts[static_cast<std::size_t>(telemetry::TcpState::LISTEN)] == 599);

			// Uncorrupted queue arithmetic across 64 KiB chunks
			assert(snap.total_tx_queue_bytes == 32);
			assert(snap.total_rx_queue_bytes == 64);
			assert(snap.max_tx_queue_bytes == 32);
			assert(snap.max_rx_queue_bytes == 64);
		}

		// POSIX equivalent of deleting the temporary file from the disk once the test finishes.
		::unlink(temp_path);
		std::cout << "  [PASS] Multi-chunk 64 KiB boundary straddle verified." << std::endl;
	}

	// --------------------------------------------------------------------------
	// Test 2c: Corrupt & Unusually Long Lines (Sanitizer & Bounds Safety)
	// Tests resilience against corrupted inputs and lines exceeding 64 KiB
	// to ensure zero buffer overruns under AddressSanitizer & UndefinedBehaviorSanitizer.
	// --------------------------------------------------------------------------
	{
		// 1. Malformed and edge-case line variants
		telemetry::TcpSnapshot snap{};
		assert(!telemetry::parse_tcp_line("", snap));
		assert(!telemetry::parse_tcp_line("   \t   ", snap));
		assert(!telemetry::parse_tcp_line("  sl  local_address rem_address   st tx_queue rx_queue\n", snap));
		assert(!telemetry::parse_tcp_line("   0: 00000000:0050", snap)); // Truncated before state
		assert(!telemetry::parse_tcp_line("   0: 00000000:0050 00000000:0000", snap));
		assert(!telemetry::parse_tcp_line("   0: 00000000:0050 00000000:0000 ZZ", snap)); // Invalid hex state
		assert(snap.socket_count == 0);

		// Malformed queues (missing colon or non-hex chars)
		// Should parse state safely without buffer overrun
		assert(telemetry::parse_tcp_line("   0: 00000000:0050 00000000:0000 01 0000002000000040", snap));
		assert(snap.state_counts[static_cast<std::size_t>(telemetry::TcpState::ESTABLISHED)] == 1);

		// 2. Unusually long line (> 70,000 chars without newline) fed into TcpCollector
		char temp_path[] = "/tmp/telemetryd_tcp_oversized_XXXXXX";
		const int temp_fd = ::mkstemp(temp_path);
		assert(temp_fd >= 0);

		const std::string_view header =
			"  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n";
		ssize_t w = ::write(temp_fd, header.data(), header.size());
		assert(w == static_cast<ssize_t>(header.size()));

		// Write a 70,000-character corrupted line with no newlines
		char junk[1024];
		std::memset(junk, 'X', sizeof(junk));
		for (int i = 0; i < 70; ++i) {
			w = ::write(temp_fd, junk, sizeof(junk));
			assert(w == static_cast<ssize_t>(sizeof(junk)));
		}
		// End the giant junk line with a newline
		const char nl = '\n';
		w = ::write(temp_fd, &nl, 1);
		assert(w == 1);

		// Followed immediately by a valid socket line
		const std::string_view valid_line =
			"   0: 0100007F:1F90 0100007F:C350 01 00000020:00000040 00:00000000 00000000  1000        0 12345 1 0000000000000000 100 0 0 10 0\n";
		w = ::write(temp_fd, valid_line.data(), valid_line.size());
		assert(w == static_cast<ssize_t>(valid_line.size()));
		::close(temp_fd);

		// Sample through TcpCollector: must not overrun buffer, must safely drop oversized line and parse valid line
		{
			telemetry::TcpCollector collector(temp_path, "/dev/null");
			const bool ok = collector.sample();
			assert(ok);

			const auto& collector_snap = collector.snapshot();
			assert(collector_snap.socket_count == 1);
			assert(collector_snap.state_counts[static_cast<std::size_t>(telemetry::TcpState::ESTABLISHED)] == 1);
			assert(collector_snap.total_tx_queue_bytes == 32);
			assert(collector_snap.total_rx_queue_bytes == 64);
		}

		::unlink(temp_path);
		std::cout << "  [PASS] Corrupt & unusually long lines verified (zero buffer overruns)." << std::endl;
	}

	// --------------------------------------------------------------------------
	// Test 3: Real /proc/net/tcp Live Collection under AllocGuard
	// --------------------------------------------------------------------------
	{
		telemetry::TcpCollector collector;
		const bool sampled = collector.sample();
		assert(sampled);

		const auto& snap = collector.snapshot();
		std::cout << "  [LIVE] Tracked " << snap.socket_count
				  << " active TCP sockets, "
				  << "tx_queue total=" << snap.total_tx_queue_bytes << " B, "
				  << "rx_queue total=" << snap.total_rx_queue_bytes << " B."
				  << std::endl;

		// Verify zero allocations over 1,000 continuous sampling iterations
		{
			AllocGuard guard;
			for(int i = 0; i < 1000; ++i) {
				const bool ok = collector.sample();
				assert(ok);
			}
			guard.assert_zero_allocations();
		}
		std::cout
			<< "  [PASS] 1,000 iterations completed with 0 heap allocations."
			<< std::endl;
	}

	// --------------------------------------------------------------------------
	// Test 4: Move semantics and RAII file descriptor management (IPv4 & IPv6)
	// --------------------------------------------------------------------------
	{
		int original_fd = -1;
		int original_fd6 = -1;
		{
			telemetry::TcpCollector collector1;
			original_fd = collector1.fd_tcp();
			original_fd6 = collector1.fd_tcp6();
			assert(original_fd >= 0);

			// Move construct
			telemetry::TcpCollector collector2(std::move(collector1));
			assert(collector1.fd_tcp() == -1);
			assert(collector1.fd_tcp6() == -1);
			assert(collector2.fd_tcp() == original_fd);
			assert(collector2.fd_tcp6() == original_fd6);

			// Move assign
			telemetry::TcpCollector collector3;
			const int third_fd = collector3.fd_tcp();
			const int third_fd6 = collector3.fd_tcp6();
			assert(third_fd >= 0);

			collector3 = std::move(collector2);
			assert(collector2.fd_tcp() == -1);
			assert(collector2.fd_tcp6() == -1);
			assert(collector3.fd_tcp() == original_fd);
			assert(collector3.fd_tcp6() == original_fd6);

			// Verify third_fd was closed by assignment
			// fcntl is the POSIX file control syscall.
			assert(::fcntl(third_fd, F_GETFD) == -1 && errno == EBADF);
			if(third_fd6 >= 0) {
				assert(::fcntl(third_fd6, F_GETFD) == -1 && errno == EBADF);
			}
		}

		// Verify original_fd was closed on destruction
		assert(::fcntl(original_fd, F_GETFD) == -1 && errno == EBADF);
		if(original_fd6 >= 0) {
			assert(::fcntl(original_fd6, F_GETFD) == -1 && errno == EBADF);
		}
		std::cout
			<< "  [PASS] RAII descriptor lifecycle and move semantics verified."
			<< std::endl;
	}

	std::cout << "[test_tcp] All tests passed successfully!" << std::endl;
	return 0;
}
