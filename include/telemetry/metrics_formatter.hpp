#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <telemetry/snapshot.hpp>

namespace telemetry {

// ==============================================================================
// MetricsFormatter: Zero-Allocation OpenMetrics / Prometheus Serializer
// ==============================================================================
//
// Formats a SystemSnapshot directly into a 64-byte cache-aligned 128 KiB member
// buffer using fast std::to_chars for numbers and std::memcpy for static labels.
// Strictly avoids sprintf, snprintf, std::stringstream, and std::to_string.
class MetricsFormatter {
   public:
	static constexpr std::size_t BUFFER_CAPACITY = 131072;	// 128 KiB

   private:
	// Output buffer: aligned to 64 bytes to prevent false sharing and optimize SIMD
	alignas(64) std::array<char, BUFFER_CAPACITY> m_buffer{};
	std::size_t m_payload_size{0};
	std::size_t m_total_size{0};

	// Helper: Fast buffer cursor for appending
	class Cursor {
	   private:
		char* m_ptr;
		char* const m_end;

	   public:
		Cursor(char* begin, char* end) noexcept : m_ptr(begin), m_end(end) {}

		// For strings
		void write_str(std::string_view sv) noexcept {
			const std::size_t remaining = static_cast<std::size_t>(m_end - m_ptr);
			if(sv.size() <= remaining) {
				std::memcpy(m_ptr, sv.data(), sv.size());
				m_ptr += sv.size();
			}
		}

		// Declare IntT as a generic type
		template <typename IntT>
		void write_int(IntT val) noexcept {
			auto [res_ptr, ec] = std::to_chars(m_ptr, m_end, val);
			if(ec == std::errc{}) {
				m_ptr = res_ptr;
			}
		}

		// For floats with custom precision
		void write_float(double val, int precision = 2) noexcept {
			// fixed => Number with a decimal point rther than scientific notation
			// precision => Number of digits after decimal point
			auto [res_ptr, ec] = std::to_chars(m_ptr, m_end, val, std::chars_format::fixed, precision);
			if(ec == std::errc{}) {
				m_ptr = res_ptr;
			}
		}

		void write_newline() noexcept {
			if(m_ptr < m_end) {
				*m_ptr++ = '\n';
			}
		}

		void write_char(char c) noexcept {
			if(m_ptr < m_end) {
				*m_ptr++ = c;
			}
		}

		char* current() const noexcept {
			return m_ptr;
		}

		// Count from custom 'start' passed!
		std::size_t count(const char* start) const noexcept {
			return static_cast<std::size_t>(m_ptr - start);
		}
	};

   public:
	MetricsFormatter() noexcept = default;

	// Non-copyable and non-movable to anchor buffer address
	MetricsFormatter(const MetricsFormatter&) = delete;
	MetricsFormatter& operator=(const MetricsFormatter&) = delete;
	MetricsFormatter(MetricsFormatter&&) = delete;
	MetricsFormatter& operator=(MetricsFormatter&&) = delete;

	// Reset buffer state
	void reset() noexcept {
		m_payload_size = 0;
		m_total_size = 0;
	}

	// Formats the SystemSnapshot into OpenMetrics line format, prepends HTTP header
	// with Content-Length, and returns the full ready-to-send response string_view.
	std::string_view format_response(const SystemSnapshot& snap) noexcept {
		reset();

		// We reserve a 256-byte header window at the start of m_buffer.
		// The payload is formatted directly at m_buffer.data() + 256.
		// Once the payload is complete, we write the HTTP header into m_buffer.data()
		// and shift the header so it connects seamlessly with the payload!
		constexpr std::size_t HEADER_RESERVE = 256;
		char* const payload_start = m_buffer.data() + HEADER_RESERVE;
		char* const payload_end = m_buffer.data() + BUFFER_CAPACITY;

		Cursor cur(payload_start, payload_end);

		// ----------------------------------------------------------------------
		// 1. CPU Metrics
		// ----------------------------------------------------------------------
		cur.write_str("# HELP host_cpu_usage_percent CPU utilization percentage per core\n");
		cur.write_str("# TYPE host_cpu_usage_percent gauge\n");

		// Aggregate CPU (core "all")
		const double agg_pct = compute_cpu_utilization(snap.cpu.m_previous[0], snap.cpu.m_current[0]);
		cur.write_str("host_cpu_usage_percent{core=\"all\"} ");
		cur.write_float(agg_pct, 2);
		cur.write_newline();

		// Per-core utilization
		// Prometheus entry example => metric_name{labels_in_key_value_pairs_with_=_and_separated_with_commas} metric_value
		for(std::size_t c = 1; c <= snap.cpu.detected_cores && c <= MAX_CORES; ++c) {
			const double core_pct = compute_cpu_utilization(snap.cpu.m_previous[c], snap.cpu.m_current[c]);
			cur.write_str("host_cpu_usage_percent{core=\"");
			cur.write_int(c - 1);
			cur.write_str("\"} ");
			cur.write_float(core_pct, 2);
			cur.write_newline();
		}

		// CPU Jiffies counters
		cur.write_str("# HELP host_cpu_jiffies_total Cumulative CPU jiffies by state\n");
		cur.write_str("# TYPE host_cpu_jiffies_total counter\n");
		// Lambda function
		auto write_jiffy_line = [&](std::string_view core_id, std::string_view state, uint64_t val) {
			cur.write_str("host_cpu_jiffies_total{core=\"");
			cur.write_str(core_id);
			cur.write_str("\",state=\"");
			cur.write_str(state);
			cur.write_str("\"} ");
			cur.write_int(val);
			cur.write_newline();
		};

		const auto& agg_jiffies = snap.cpu.m_current[0];
		write_jiffy_line("all", "user", agg_jiffies.user);
		write_jiffy_line("all", "system", agg_jiffies.system);
		write_jiffy_line("all", "idle", agg_jiffies.idle);
		write_jiffy_line("all", "iowait", agg_jiffies.iowait);

		// ----------------------------------------------------------------------
		// 2. Memory Metrics (Converted to bytes from KiB)
		// ----------------------------------------------------------------------
		cur.write_str("# HELP host_memory_bytes Linux memory counters in bytes\n");
		cur.write_str("# TYPE host_memory_bytes gauge\n");
		auto write_mem_line = [&](std::string_view type, uint64_t kib) {
			cur.write_str("host_memory_bytes{type=\"");
			cur.write_str(type);
			cur.write_str("\"} ");
			cur.write_int(kib * 1024ULL);  // Convert KiB to bytes
			cur.write_newline();
		};

		write_mem_line("total", snap.mem.mem_total);
		write_mem_line("free", snap.mem.mem_free);
		write_mem_line("available", snap.mem.mem_available);
		write_mem_line("buffers", snap.mem.buffers);
		write_mem_line("cached", snap.mem.cached);
		write_mem_line("swap_total", snap.mem.swap_total);
		write_mem_line("swap_free", snap.mem.swap_free);

		// ----------------------------------------------------------------------
		// 3. TCP Metrics
		// ----------------------------------------------------------------------
		cur.write_str("# HELP host_tcp_sockets_state Active TCP sockets categorized by kernel state\n");
		cur.write_str("# TYPE host_tcp_sockets_state gauge\n");
		auto write_tcp_state_line = [&](std::string_view state_str, uint64_t count) {
			cur.write_str("host_tcp_sockets_state{state=\"");
			cur.write_str(state_str);
			cur.write_str("\"} ");
			cur.write_int(count);
			cur.write_newline();
		};

		write_tcp_state_line("established", snap.tcp.state_counts[static_cast<std::size_t>(TcpState::ESTABLISHED)]);
		write_tcp_state_line("syn_sent", snap.tcp.state_counts[static_cast<std::size_t>(TcpState::SYN_SENT)]);
		write_tcp_state_line("syn_recv", snap.tcp.state_counts[static_cast<std::size_t>(TcpState::SYN_RECV)]);
		write_tcp_state_line("fin_wait1", snap.tcp.state_counts[static_cast<std::size_t>(TcpState::FIN_WAIT1)]);
		write_tcp_state_line("fin_wait2", snap.tcp.state_counts[static_cast<std::size_t>(TcpState::FIN_WAIT2)]);
		write_tcp_state_line("time_wait", snap.tcp.state_counts[static_cast<std::size_t>(TcpState::TIME_WAIT)]);
		write_tcp_state_line("close", snap.tcp.state_counts[static_cast<std::size_t>(TcpState::CLOSE)]);
		write_tcp_state_line("close_wait", snap.tcp.state_counts[static_cast<std::size_t>(TcpState::CLOSE_WAIT)]);
		write_tcp_state_line("last_ack", snap.tcp.state_counts[static_cast<std::size_t>(TcpState::LAST_ACK)]);
		write_tcp_state_line("listen", snap.tcp.state_counts[static_cast<std::size_t>(TcpState::LISTEN)]);
		write_tcp_state_line("closing", snap.tcp.state_counts[static_cast<std::size_t>(TcpState::CLOSING)]);

		cur.write_str("# HELP host_tcp_queue_bytes Aggregate TCP queue backpressure in bytes\n");
		cur.write_str("# TYPE host_tcp_queue_bytes gauge\n");
		cur.write_str("host_tcp_queue_bytes{direction=\"tx\"} ");
		cur.write_int(snap.tcp.total_tx_queue_bytes);
		cur.write_newline();
		cur.write_str("host_tcp_queue_bytes{direction=\"rx\"} ");
		cur.write_int(snap.tcp.total_rx_queue_bytes);
		cur.write_newline();

		// ----------------------------------------------------------------------
		// 4. Telemetry Scrape Metadata
		// ----------------------------------------------------------------------
		cur.write_str("# HELP host_scrape_sequence_number_total Monotonic collection tick sequence number\n");
		cur.write_str("# TYPE host_scrape_sequence_number_total counter\n");
		cur.write_str("host_scrape_sequence_number_total ");
		cur.write_int(snap.sequence_number);
		cur.write_newline();

		cur.write_str("# HELP host_scrape_duration_seconds Time spent in kernel proc collectors\n");
		cur.write_str("# TYPE host_scrape_duration_seconds gauge\n");
		cur.write_str("host_scrape_duration_seconds ");
		const double duration_sec = static_cast<double>(snap.collection_duration_ns) / 1'000'000'000.0;
		cur.write_float(duration_sec, 6);
		cur.write_newline();

		// Compute payload size
		m_payload_size = cur.count(payload_start);

		// ----------------------------------------------------------------------
		// 5. Prepend HTTP Response Header
		// ----------------------------------------------------------------------
		// Format:
		// HTTP/1.1 200 OK\r\n
		// Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n
		// Content-Length: <LEN>\r\n
		// Connection: close\r\n
		// \r\n
		std::array<char, 256> header_buf{};
		Cursor h_cur(header_buf.data(), header_buf.data() + header_buf.size());
		h_cur.write_str("HTTP/1.1 200 OK\r\nContent-Type: text/plain; version=0.0.4; charset=utf-8\r\nContent-Length: ");
		h_cur.write_int(m_payload_size);
		h_cur.write_str("\r\nConnection: close\r\n\r\n");

		const std::size_t header_len = h_cur.count(header_buf.data());

		// Place header directly before payload:
		// Start position in m_buffer is (HEADER_RESERVE - header_len)
		char* const response_start = payload_start - header_len;
		std::memcpy(response_start, header_buf.data(), header_len);

		m_total_size = header_len + m_payload_size;
		return std::string_view(response_start, m_total_size);
	}

	std::size_t payload_size() const noexcept {
		return m_payload_size;
	}

	std::size_t total_size() const noexcept {
		return m_total_size;
	}
};

}  // namespace telemetry
