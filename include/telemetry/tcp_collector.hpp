#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <system_error>
#include <telemetry/common.hpp>
#include <utility>

namespace telemetry {

// ==============================================================================
// 2. Linux TCP State Enum & Aggregation Model
// ==============================================================================

// Direct mapping of the Linux kernel TCP state machine (from
// <net/tcp_states.h>) The names stay inside their class box. 0x00 ... 0x0C are
// integers in hexadecimal!
enum class TcpState : uint8_t {
	UNKNOWN = 0x00,
	ESTABLISHED = 0x01,
	SYN_SENT = 0x02,
	SYN_RECV = 0x03,
	FIN_WAIT1 = 0x04,
	FIN_WAIT2 = 0x05,
	TIME_WAIT = 0x06,
	CLOSE = 0x07,
	CLOSE_WAIT = 0x08,
	LAST_ACK = 0x09,
	LISTEN = 0x0A,
	CLOSING = 0x0B,
	NEW_SYN_RECV = 0x0C
};

// Total distinct TCP states supported (0x01 to 0x0C + 1 sentinel slot)
constexpr std::size_t TCP_STATE_COUNT = 13;

// Returns the canonical OpenMetrics / Prometheus label string for each TCP
// state
constexpr std::string_view tcp_state_to_string(TcpState state) noexcept {
	switch(state) {
		case TcpState::ESTABLISHED:
			return "ESTABLISHED";
		case TcpState::SYN_SENT:
			return "SYN_SENT";
		case TcpState::SYN_RECV:
			return "SYN_RECV";
		case TcpState::FIN_WAIT1:
			return "FIN_WAIT1";
		case TcpState::FIN_WAIT2:
			return "FIN_WAIT2";
		case TcpState::TIME_WAIT:
			return "TIME_WAIT";
		case TcpState::CLOSE:
			return "CLOSE";
		case TcpState::CLOSE_WAIT:
			return "CLOSE_WAIT";
		case TcpState::LAST_ACK:
			return "LAST_ACK";
		case TcpState::LISTEN:
			return "LISTEN";
		case TcpState::CLOSING:
			return "CLOSING";
		case TcpState::NEW_SYN_RECV:
			return "NEW_SYN_RECV";
		case TcpState::UNKNOWN:
		default:
			return "UNKNOWN";
	}
}

// ==============================================================================
// Aggregated Metrics Struct (TcpSnapshot)
// ==============================================================================

// Flat struct tracking host-wide network socket health with zero dynamic
// allocations.
struct TcpSnapshot {
	// Array indexed by the TCP state integer (0x01 to 0x0C) storing socket
	// counts per state
	std::array<uint64_t, TCP_STATE_COUNT> state_counts{};

	// Cumulative bytes buffered in kernel transmission queues across all
	// sockets
	uint64_t total_tx_queue_bytes{0};

	// Cumulative bytes buffered in kernel receive queues across all sockets
	uint64_t total_rx_queue_bytes{0};

	// Largest single socket tx_queue size observed (detects single-connection
	// head-of-line blocking)
	uint64_t max_tx_queue_bytes{0};

	// Largest single socket rx_queue size observed (detects process ingestion
	// stalls)
	uint64_t max_rx_queue_bytes{0};

	// Total active TCP sockets tracked
	uint64_t socket_count{0};

	// Resets all aggregated counters to zero before a new sampling pass
	void reset() noexcept {
		state_counts.fill(0);
		total_tx_queue_bytes = 0;
		total_rx_queue_bytes = 0;
		max_tx_queue_bytes = 0;
		max_rx_queue_bytes = 0;
		socket_count = 0;
	}
};

// ==============================================================================
// 3. In-Place Hex Parsing Algorithm
// ==============================================================================

// Parses a single line from /proc/net/tcp.
// Avoids std::stringstream, sscanf, or regex. Relies strictly on pointer
// arithmetic and std::from_chars with base-16.
inline bool parse_tcp_line(std::string_view line,
						   TcpSnapshot& snapshot) noexcept {
	if(line.empty()) {
		return false;
	}

	const char* ptr = line.data();
	const char* const end = line.data() + line.size();

	// Skip leading whitespace
	while(ptr < end && (*ptr == ' ' || *ptr == '\t')) {
		++ptr;
	}
	if(ptr >= end) {
		return false;
	}

	// 1. Header Skipping: If this line contains column headers (e.g. "sl" or
	// "local_address"), skip it
	if(line.find("local_address") != std::string_view::npos) {
		return false;
	}

	// 2. Column Navigation via Character Scanning:
	// Skip row index (sl:)
	while(ptr < end && *ptr != ' ' && *ptr != '\t') ++ptr;
	while(ptr < end && (*ptr == ' ' || *ptr == '\t')) ++ptr;
	if(ptr >= end) return false;

	// Skip local_address (XXXXXXXX:XXXX)
	while(ptr < end && *ptr != ' ' && *ptr != '\t') ++ptr;
	while(ptr < end && (*ptr == ' ' || *ptr == '\t')) ++ptr;
	if(ptr >= end) return false;

	// Skip rem_address (XXXXXXXX:XXXX)
	while(ptr < end && *ptr != ' ' && *ptr != '\t') ++ptr;
	while(ptr < end && (*ptr == ' ' || *ptr == '\t')) ++ptr;
	if(ptr >= end) return false;

	// 3. State Decoding: Parse the 2 hex characters in the 'st' column
	if(ptr + 2 > end) {
		return false;
	}
	uint8_t state_val = 0;
	const auto [state_next, state_ec] =
		std::from_chars(ptr, ptr + 2, state_val, 16);
	if(state_ec != std::errc()) {
		return false;
	}

	if(state_val < TCP_STATE_COUNT) {
		snapshot.state_counts[state_val]++;
	} else {
		snapshot.state_counts[0]++;	 // UNKNOWN
	}
	snapshot.socket_count++;
	ptr = state_next;

	// 4. Queue Pressure Decoding (tx_queue:rx_queue)
	// Skip whitespace to reach queue field (XXXXXXXX:XXXXXXXX)
	while(ptr < end && (*ptr == ' ' || *ptr == '\t')) {
		++ptr;
	}
	if(ptr >= end) {
		return true;
	}

	// Locate the colon ':' separating tx_queue and rx_queue
	const char* colon_ptr = ptr;
	while(colon_ptr < end && *colon_ptr != ':' && *colon_ptr != ' ' &&
		  *colon_ptr != '\t') {
		++colon_ptr;
	}
	if(colon_ptr >= end || *colon_ptr != ':') {
		return true;
	}

	// Parse tx_queue using std::from_chars(ptr, colon_ptr, tx_bytes, 16)
	uint64_t tx_bytes = 0;
	const auto [tx_next, tx_ec] = std::from_chars(ptr, colon_ptr, tx_bytes, 16);
	if(tx_ec != std::errc()) {
		return true;
	}

	// Advance past the colon ':'
	const char* rx_start = colon_ptr + 1;
	const char* space_ptr = rx_start;
	while(space_ptr < end && *space_ptr != ' ' && *space_ptr != '\t') {
		++space_ptr;
	}

	// Parse rx_queue using std::from_chars(colon_ptr + 1, space_ptr, rx_bytes,
	// 16)
	uint64_t rx_bytes = 0;
	const auto [rx_next, rx_ec] =
		std::from_chars(rx_start, space_ptr, rx_bytes, 16);
	if(rx_ec != std::errc()) {
		return true;
	}

	// Accumulate total queue bytes
	snapshot.total_tx_queue_bytes += tx_bytes;
	snapshot.total_rx_queue_bytes += rx_bytes;

	// Update max_tx_queue_bytes and max_rx_queue_bytes
	snapshot.max_tx_queue_bytes =
		std::max(snapshot.max_tx_queue_bytes, tx_bytes);
	snapshot.max_rx_queue_bytes =
		std::max(snapshot.max_rx_queue_bytes, rx_bytes);

	// 5. Line Advance:
	// Once the queue pressure fields are parsed, stop scanning the line
	// immediately! The remaining columns (tr, tm->when, retrnsmt, uid, timeout,
	// inode) are not needed for host socket health telemetry. By exiting here,
	// LineCursor advances directly to the next newline '\n', minimizing cache
	// line parsing and CPU work.
	return true;
}

// Parses a multi-line buffer of /proc/net/tcp data line by line
inline void parse_tcp_buffer(std::string_view buffer,
							 TcpSnapshot& snapshot) noexcept {
	LineCursor cursor(buffer);
	std::string_view line;
	while(cursor.next_line(line)) {
		parse_tcp_line(line, snapshot);
	}
}

// ==============================================================================
// 4. File Buffer Sizing, Chunking & Line Boundary Splicing (TcpCollector)
// ==============================================================================

// RAII Collector for TCP socket metrics (/proc/net/tcp and /proc/net/tcp6).
// Handles large socket tables (10,000+ connections producing ~1.5 MiB of
// virtual text) using a fixed 64 KiB reusable buffer, chunked sequential reads,
// and line boundary splicing across both IPv4 and IPv6 stacks.
class TcpCollector {
   public:
	// Fixed buffer capacity: 64 KiB cache-aligned buffer held as member
	// variable
	static constexpr std::size_t BUFFER_SIZE = 65536;

	// Constructor: Opens persistent descriptors to both IPv4 and IPv6 paths
	// with O_RDONLY | O_CLOEXEC
	explicit TcpCollector(const char* tcp_path = "/proc/net/tcp",
						  const char* tcp6_path = "/proc/net/tcp6") {
		m_fd_tcp = ::open(tcp_path, O_RDONLY | O_CLOEXEC);
		if(m_fd_tcp < 0) {
			throw std::system_error(
				errno, std::generic_category(),
				"TcpCollector: failed to open /proc/net/tcp");
		}

		m_fd_tcp6 = ::open(tcp6_path, O_RDONLY | O_CLOEXEC);
		// Note: tcp6 may not exist on systems where IPv6 is disabled in the kernel
	}

	// Destructor: RAII lifecycle discipline, automatically closes descriptors
	~TcpCollector() noexcept {
		if(m_fd_tcp >= 0) {
			::close(m_fd_tcp);
			m_fd_tcp = -1;
		}
		if(m_fd_tcp6 >= 0) {
			::close(m_fd_tcp6);
			m_fd_tcp6 = -1;
		}
	}

	// Non-copyable to prevent duplicate file descriptor ownership
	TcpCollector(const TcpCollector&) = delete;
	TcpCollector& operator=(const TcpCollector&) = delete;

	// Movable: transfers file descriptor ownership cleanly
	TcpCollector(TcpCollector&& other) noexcept
		: m_read_buffer(other.m_read_buffer),
		  m_snapshot(other.m_snapshot),
		  m_fd_tcp(other.m_fd_tcp),
		  m_fd_tcp6(other.m_fd_tcp6) {
		other.m_fd_tcp = -1;
		other.m_fd_tcp6 = -1;
	}

	TcpCollector& operator=(TcpCollector&& other) noexcept {
		if(this != &other) {
			if(m_fd_tcp >= 0) {
				::close(m_fd_tcp);
			}
			if(m_fd_tcp6 >= 0) {
				::close(m_fd_tcp6);
			}
			m_read_buffer = other.m_read_buffer;
			m_snapshot = other.m_snapshot;
			m_fd_tcp = other.m_fd_tcp;
			m_fd_tcp6 = other.m_fd_tcp6;
			other.m_fd_tcp = -1;
			other.m_fd_tcp6 = -1;
		}
		return *this;
	}

	// sample():
	// - Resets m_snapshot.
	// - Samples IPv4 (/proc/net/tcp) via m_fd_tcp.
	// - Samples IPv6 (/proc/net/tcp6) via m_fd_tcp6 and aggregates into m_snapshot.
	bool sample() noexcept {
		if(m_fd_tcp < 0 && m_fd_tcp6 < 0) {
			return false;
		}

		m_snapshot.reset();

		bool success = false;
		if(m_fd_tcp >= 0) {
			success = sample_stream(m_fd_tcp);
		}
		// Stream & parse IPv6 (/proc/net/tcp6) INTO THE SAME SNAPSHOT!
		if(m_fd_tcp6 >= 0) {
			const bool ok6 = sample_stream(m_fd_tcp6);
			success = success || ok6;
		}

		return success;
	}

	// Convenience alias for sample()
	bool collect() noexcept { return sample(); }

	// Accessors: Non-allocating const getters returning references or scalar values
	const TcpSnapshot& snapshot() const noexcept { return m_snapshot; }
	int fd() const noexcept { return m_fd_tcp; }
	int fd_tcp() const noexcept { return m_fd_tcp; }
	int fd_tcp6() const noexcept { return m_fd_tcp6; }

   private:
	// Streams and parses socket metrics from a single descriptor (IPv4 or IPv6)
	bool sample_stream(int fd) noexcept {
		if(fd < 0) {
			return false;
		}

		// Rewind descriptor offset to start of file for current sampling tick
		if(::lseek(fd, 0, SEEK_SET) == -1) {
			return false;
		}

		std::size_t tail_len = 0;

		while(true) {
			// That means our buffer is 100% full of a single, continuous line
			// that has never hit a \n newline character across multiple 64 KiB
			// reads! Normal /proc/net/tcp lines are only ~150 bytes.
			if(tail_len >= m_read_buffer.data.size() - 1) {
				// Defensive fallback: individual line exceeded 64 KiB buffer
				// without newline => GARBAGE => Discard it!
				tail_len = 0;
			}

			// Read next chunk into buffer after leftover tail
			// tail_len bytes are already sitting at the beginning of the
			// buffer and 1 is reserved for \0!
			const std::size_t available_space =
				(m_read_buffer.data.size() - 1) - tail_len;

			const ssize_t bytes_read =
				::read(fd, m_read_buffer.data.data() + tail_len, available_space);
			if(bytes_read < 0) {
				if(errno == EINTR) {
					continue;  // Interrupted by signal, retry read
				}
				return false;
			}

			if(bytes_read == 0) {
				// EOF reached: if there's any remaining valid line without
				// trailing newline, parse it
				if(tail_len > 0) {
					m_read_buffer.data[tail_len] = '\0';
					parse_tcp_buffer(
						std::string_view(m_read_buffer.data.data(), tail_len),
						m_snapshot);
				}
				break;
			}

			const std::size_t total_bytes =
				tail_len + static_cast<std::size_t>(bytes_read);
			m_read_buffer.data[total_bytes] = '\0';
			m_read_buffer.size = total_bytes;

			// Find last newline character in current chunk to locate the clean
			// line boundary
			const std::string_view chunk_view(m_read_buffer.data.data(), total_bytes);
			const auto last_nl = chunk_view.rfind('\n');

			if(last_nl != std::string_view::npos) {
				// Parse all complete lines up to and including last newline
				const std::string_view complete_lines =
					chunk_view.substr(0, last_nl + 1);
				parse_tcp_buffer(complete_lines, m_snapshot);

				// Shift remaining partial tail bytes to the front of buffer
				const std::size_t next_tail_len = total_bytes - (last_nl + 1);
				if(next_tail_len > 0) {
					// (dest, src, size)
					std::memmove(m_read_buffer.data.data(),
								 m_read_buffer.data.data() + last_nl + 1,
								 next_tail_len);
				}
				tail_len = next_tail_len;
			} else {
				// No newline found yet in chunk: keep accumulated tail bytes
				// and continue reading
				tail_len = total_bytes;
				if(tail_len >= m_read_buffer.data.size() - 1) {
					// Drop oversized line to prevent buffer overflow
					tail_len = 0;
				}
			}
		}

		return true;
	}

	ScratchBuffer<BUFFER_SIZE> m_read_buffer{};
	TcpSnapshot m_snapshot{};
	int m_fd_tcp{-1};
	int m_fd_tcp6{-1};
};

}  // namespace telemetry
