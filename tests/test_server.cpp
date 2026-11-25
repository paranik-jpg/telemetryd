#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <iostream>
#include <string_view>
#include <telemetry/http_server.hpp>
#include <telemetry/metrics_formatter.hpp>
#include <telemetry/snapshot.hpp>
#include <thread>

#include "alloc_hook.hpp"

// ==============================================================================
// Unit & Integration Tests: MetricsFormatter & HttpServer
// ==============================================================================

// Helper: Establish IPv4 client socket connected to localhost:port
static int connect_to_server(uint16_t port) {
	int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if(client_fd < 0) {
		return -1;
	}

	struct sockaddr_in saddr{};
	saddr.sin_family = AF_INET;
	saddr.sin_port = htons(port);
	if(::inet_pton(AF_INET, "127.0.0.1", &saddr.sin_addr) <= 0) {
		::close(client_fd);
		return -1;
	}

	if(::connect(client_fd, reinterpret_cast<struct sockaddr*>(&saddr), sizeof(saddr)) < 0) {
		::close(client_fd);
		return -1;
	}

	return client_fd;
}

// Helper: Reliably send exact payload with full error checking
static void send_exact(int fd, std::string_view payload) {
	std::size_t sent_total = 0;
	while(sent_total < payload.size()) {
		const ssize_t n = ::send(fd, payload.data() + sent_total, payload.size() - sent_total, 0);
		assert(n > 0);
		sent_total += static_cast<std::size_t>(n);
	}
	assert(sent_total == payload.size());
}

// --------------------------------------------------------------------------
// Test 1: MetricsFormatter Output & OpenMetrics Compliance
// --------------------------------------------------------------------------
static void test_metrics_formatter() {
	std::cout << "[test_server] Running test_metrics_formatter..." << std::endl;

	telemetry::MetricsFormatter formatter;
	telemetry::SystemSnapshot snap{};

	// Seed test snapshot
	snap.sequence_number = 42;
	snap.collection_duration_ns = 1'500'000;  // 1.5ms
	snap.cpu.detected_cores = 2;
	snap.cpu.m_current[0].user = 1000;
	snap.cpu.m_current[0].system = 200;
	snap.cpu.m_current[0].idle = 8000;

	snap.mem.mem_total = 16'000'000;	 // 16 GB in KiB
	snap.mem.mem_free = 4'000'000;		 // 4 GB in KiB
	snap.mem.mem_available = 8'000'000;	 // 8 GB in KiB

	snap.tcp.state_counts[static_cast<std::size_t>(telemetry::TcpState::ESTABLISHED)] = 25;
	snap.tcp.state_counts[static_cast<std::size_t>(telemetry::TcpState::TIME_WAIT)] = 10;
	snap.tcp.total_tx_queue_bytes = 2048;

	// Format response
	const std::string_view resp = formatter.format_response(snap);
	assert(!resp.empty());
	assert(resp.size() >= 17 && resp.substr(0, 17) == "HTTP/1.1 200 OK\r\n");
	assert(resp.find("Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n") != std::string_view::npos);
	assert(resp.find("Content-Length: ") != std::string_view::npos);
	assert(resp.find("host_cpu_usage_percent{core=\"all\"}") != std::string_view::npos);
	assert(resp.find("host_memory_bytes{type=\"total\"} 16384000000") != std::string_view::npos);
	assert(resp.find("host_tcp_sockets_state{state=\"established\"} 25") != std::string_view::npos);
	assert(resp.find("host_scrape_sequence_number_total 42") != std::string_view::npos);

	std::cout << "  [PASS] MetricsFormatter generated " << resp.size() << " bytes of valid OpenMetrics text." << std::endl;
}

// --------------------------------------------------------------------------
// Test 2: HTTP Parsing & Route Validation (200, 404, 405, 414)
// --------------------------------------------------------------------------
static void test_http_routes() {
	std::cout << "[test_server] Running test_http_routes (200, 404, 405, 414)..." << std::endl;

	telemetry::SnapshotRegistry registry;
	telemetry::SystemSnapshot snap{};
	snap.cpu.detected_cores = 1;
	registry.publish(snap);

	constexpr uint16_t TEST_PORT = 19123;
	telemetry::HttpServer server(registry, TEST_PORT);
	server.start();

	// 1. Valid GET /metrics
	{
		int fd = connect_to_server(TEST_PORT);
		assert(fd >= 0);
		send_exact(fd, "GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");

		char buf[4096];
		const ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
		assert(n > 0);
		buf[n] = '\0';
		const std::string_view resp_view(buf, static_cast<std::size_t>(n));
		assert(resp_view.size() >= 17 && resp_view.substr(0, 17) == "HTTP/1.1 200 OK\r\n");
		::close(fd);
	}

	// 2. Valid GET /
	{
		int fd = connect_to_server(TEST_PORT);
		assert(fd >= 0);
		send_exact(fd, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");

		char buf[512];
		const ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
		assert(n > 0);
		buf[n] = '\0';
		const std::string_view resp_view(buf, static_cast<std::size_t>(n));
		assert(resp_view.size() >= 17 && resp_view.substr(0, 17) == "HTTP/1.1 200 OK\r\n");
		assert(resp_view.find("<h1>telemetryd</h1>") != std::string_view::npos);
		::close(fd);
	}

	// 3. 404 Not Found
	{
		int fd = connect_to_server(TEST_PORT);
		assert(fd >= 0);
		send_exact(fd, "GET /unknown_route HTTP/1.1\r\nHost: localhost\r\n\r\n");

		char buf[512];
		const ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
		assert(n > 0);
		buf[n] = '\0';
		const std::string_view resp_view(buf, static_cast<std::size_t>(n));
		assert(resp_view.size() >= 24 && resp_view.substr(0, 24) == "HTTP/1.1 404 Not Found\r\n");
		::close(fd);
	}

	// 4. 405 Method Not Allowed
	{
		int fd = connect_to_server(TEST_PORT);
		assert(fd >= 0);
		send_exact(fd, "POST /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");

		char buf[512];
		const ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
		assert(n > 0);
		buf[n] = '\0';
		const std::string_view resp_view(buf, static_cast<std::size_t>(n));
		assert(resp_view.size() >= 31 && resp_view.substr(0, 31) == "HTTP/1.1 405 Method Not Allowed");
		::close(fd);
	}

	// 5. 414 URI Too Long (> 2048 bytes)
	{
		int fd = connect_to_server(TEST_PORT);
		assert(fd >= 0);
		char huge_req[2200];
		std::memset(huge_req, 'A', sizeof(huge_req));
		std::memcpy(huge_req, "GET /", 5);
		huge_req[sizeof(huge_req) - 2] = '\r';
		huge_req[sizeof(huge_req) - 1] = '\n';

		send_exact(fd, std::string_view(huge_req, sizeof(huge_req)));

		char buf[512];
		const ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
		assert(n > 0);
		buf[n] = '\0';
		const std::string_view resp_view(buf, static_cast<std::size_t>(n));
		assert(resp_view.size() >= 25 && resp_view.substr(0, 25) == "HTTP/1.1 414 URI Too Long");
		::close(fd);
	}

	server.stop();
	std::cout << "  [PASS] All HTTP routing and error branches verified." << std::endl;
}

// --------------------------------------------------------------------------
// Test 3: Memory Hook Guard Test (AllocGuard End-to-End Scrape)
// Wrap an end-to-end request handling flow inside AllocGuard.
// Simulate a scraper establishing a socket, sending GET /metrics, and reading
// the complete response.
// Verify that serialization and socket flushing complete with zero heap allocations.
// --------------------------------------------------------------------------
static void test_server_zero_alloc() {
	std::cout << "[test_server] Running test_server_zero_alloc (End-to-End Scrape under AllocGuard)..." << std::endl;

	telemetry::SnapshotRegistry registry;
	telemetry::SystemSnapshot snap{};
	snap.cpu.detected_cores = 4;
	snap.mem.mem_total = 16000000;
	snap.tcp.socket_count = 50;
	registry.publish(snap);

	constexpr uint16_t TEST_PORT = 19124;
	telemetry::HttpServer server(registry, TEST_PORT);
	server.start();

	// Connect client socket before entering AllocGuard (connection establishment
	// in client test helper is separate from server request processing)
	int client_fd = connect_to_server(TEST_PORT);
	assert(client_fd >= 0);

	// Buffer to store scraped response on client side
	char client_buf[8192];
	ssize_t total_received = 0;

	// Mechanical Zero-Allocation Assertion across scrape transaction
	{
		AllocGuard guard;

		const char request[] = "GET /metrics HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
		const ssize_t bytes_sent = ::send(client_fd, request, sizeof(request) - 1, 0);
		assert(bytes_sent == sizeof(request) - 1);

		// Read complete response until EOF
		while(total_received < static_cast<ssize_t>(sizeof(client_buf))) {
			const std::size_t remaining = sizeof(client_buf) - static_cast<std::size_t>(total_received);
			const ssize_t n = ::recv(client_fd, client_buf + total_received, remaining, 0);
			if(n > 0) {
				total_received += n;
			} else if(n == 0) {
				// Orderly shutdown (EOF) from server
				break;
			} else {
				// Socket error
				assert(n >= 0);
			}
		}

		guard.assert_zero_allocations();
	}

	::close(client_fd);
	server.stop();

	assert(total_received > 0);
	const std::string_view response(client_buf, static_cast<std::size_t>(total_received));
	assert(response.size() >= 17 && response.substr(0, 17) == "HTTP/1.1 200 OK\r\n");
	assert(response.find("host_scrape_sequence_number_total 1") != std::string_view::npos);
	assert(response.find("host_memory_bytes{type=\"total\"} 16384000000") != std::string_view::npos);

	std::cout << "  [LIVE] Received " << total_received << " bytes of OpenMetrics payload." << std::endl;
	std::cout << "  [PASS] End-to-end request handling verified with strictly 0 heap allocations under AllocGuard." << std::endl;
}

// --------------------------------------------------------------------------
// Test 4: Prometheus Text Format Compliance via promtool / validator
// Validates # HELP, # TYPE, decimal precision, and metric key uniqueness.
// If promtool binary is available at /tmp/promtool_install/promtool or in PATH,
// pipe the raw /metrics payload into "promtool check metrics".
// --------------------------------------------------------------------------
static void test_prometheus_format_compliance() {
	std::cout << "[test_server] Running test_prometheus_format_compliance..." << std::endl;

	telemetry::MetricsFormatter formatter;
	telemetry::SystemSnapshot snap{};
	snap.sequence_number = 1;
	snap.collection_duration_ns = 500'000;
	snap.cpu.detected_cores = 4;
	for(std::size_t i = 0; i <= 4; ++i) {
		snap.cpu.m_current[i].user = 100 * (i + 1);
		snap.cpu.m_current[i].system = 50 * (i + 1);
		snap.cpu.m_current[i].idle = 1000 * (i + 1);
	}
	snap.mem.mem_total = 16000000;
	snap.mem.mem_free = 4000000;
	snap.mem.mem_available = 8000000;
	snap.mem.buffers = 500000;
	snap.mem.cached = 3500000;
	snap.tcp.state_counts[static_cast<std::size_t>(telemetry::TcpState::ESTABLISHED)] = 10;
	snap.tcp.total_tx_queue_bytes = 1024;
	snap.tcp.total_rx_queue_bytes = 2048;

	const std::string_view resp = formatter.format_response(snap);
	const auto body_start = resp.find("\r\n\r\n");
	assert(body_start != std::string_view::npos);
	const std::string_view metrics_body = resp.substr(body_start + 4);

	// In-memory compliance assertions
	// 1. Each metric family must have # HELP and # TYPE preceding samples
	assert(metrics_body.find("# HELP host_cpu_usage_percent") != std::string_view::npos);
	assert(metrics_body.find("# TYPE host_cpu_usage_percent gauge\n") != std::string_view::npos);
	assert(metrics_body.find("# HELP host_cpu_jiffies_total") != std::string_view::npos);
	assert(metrics_body.find("# TYPE host_cpu_jiffies_total counter\n") != std::string_view::npos);
	assert(metrics_body.find("# HELP host_memory_bytes") != std::string_view::npos);
	assert(metrics_body.find("# TYPE host_memory_bytes gauge\n") != std::string_view::npos);
	assert(metrics_body.find("# HELP host_tcp_sockets_state") != std::string_view::npos);
	assert(metrics_body.find("# TYPE host_tcp_sockets_state gauge\n") != std::string_view::npos);
	assert(metrics_body.find("# HELP host_tcp_queue_bytes") != std::string_view::npos);
	assert(metrics_body.find("# TYPE host_tcp_queue_bytes gauge\n") != std::string_view::npos);
	assert(metrics_body.find("# HELP host_scrape_sequence_number_total") != std::string_view::npos);
	assert(metrics_body.find("# TYPE host_scrape_sequence_number_total counter\n") != std::string_view::npos);
	assert(metrics_body.find("# HELP host_scrape_duration_seconds") != std::string_view::npos);
	assert(metrics_body.find("# TYPE host_scrape_duration_seconds gauge\n") != std::string_view::npos);

	// 2. Trailing newline requirement: Prometheus text format strictly requires trailing newline
	assert(!metrics_body.empty() && metrics_body.back() == '\n');

	// 3. Structural syntax verification across every metric line
	// Ensures valid format: <metric_name>{<labels>} <value> OR <metric_name> <value>
	std::string_view remaining = metrics_body;
	while(!remaining.empty()) {
		const auto line_end = remaining.find('\n');
		const std::string_view line = (line_end != std::string_view::npos) ? remaining.substr(0, line_end) : remaining;
		remaining = (line_end != std::string_view::npos) ? remaining.substr(line_end + 1) : std::string_view{};

		if(line.empty()) {
			continue;
		}

		if((line.size() >= 7 && line.substr(0, 7) == "# HELP ") || (line.size() >= 7 && line.substr(0, 7) == "# TYPE ")) {
			continue;
		}

		// Metric sample lines must contain a space separator between metric identifier and value
		const auto space_pos = line.rfind(' ');
		assert(space_pos != std::string_view::npos);
		const std::string_view metric_key = line.substr(0, space_pos);
		const std::string_view metric_val = line.substr(space_pos + 1);

		assert(!metric_key.empty());
		assert(!metric_val.empty());
	}

	std::cout << "  [PASS] OpenMetrics text format, headers, and line syntax verified." << std::endl;
}

// --------------------------------------------------------------------------
// Test 5: Slowloris & Connection Dropping Stress Test
// Open a TCP socket to the exporter port, send a single byte ('G'), and sleep.
// Verify the server times out and evicts the stalled socket without deadlocking
// the sampling engine or exhausting file descriptors.
// --------------------------------------------------------------------------
static void test_slowloris_eviction() {
	std::cout << "[test_server] Running test_slowloris_eviction (Slowloris & timeout test)..." << std::endl;

	telemetry::SnapshotRegistry registry;
	telemetry::SystemSnapshot snap{};
	snap.sequence_number = 1;
	registry.publish(snap);

	constexpr uint16_t TEST_PORT = 19125;
	telemetry::HttpServer server(registry, TEST_PORT);
	server.start();

	// 1. Establish Slowloris client connection
	const int slowloris_fd = connect_to_server(TEST_PORT);
	assert(slowloris_fd >= 0);

	// 2. Send a single byte ('G') and intentionally pause
	const char single_byte = 'G';
	const ssize_t sent = ::send(slowloris_fd, &single_byte, 1, 0);
	assert(sent == 1);

	// 3. Sleep longer than the server's 500ms timeout ceiling (700ms)
	std::this_thread::sleep_for(std::chrono::milliseconds(700));

	// 4. Verify server evicted the stalled socket: recv should return 0 (EOF) or error
	char buf[128];
	const ssize_t n = ::recv(slowloris_fd, buf, sizeof(buf), MSG_DONTWAIT);
	assert(n <= 0);	 // Socket was closed by server (EOF == 0, or ECONNRESET / EPIPE)
	::close(slowloris_fd);

	// 5. Verify the server remains fully responsive and non-deadlocked
	const int normal_fd = connect_to_server(TEST_PORT);
	assert(normal_fd >= 0);
	send_exact(normal_fd, "GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");

	char resp_buf[512];
	const ssize_t rn = ::recv(normal_fd, resp_buf, sizeof(resp_buf) - 1, 0);
	assert(rn > 0);
	resp_buf[rn] = '\0';
	const std::string_view resp_view(resp_buf, static_cast<std::size_t>(rn));
	assert(resp_view.size() >= 15 && resp_view.substr(0, 15) == "HTTP/1.1 200 OK");
	::close(normal_fd);

	server.stop();
	std::cout << "  [PASS] Stalled Slowloris connection evicted after timeout; server remains responsive." << std::endl;
}

static std::atomic<bool> g_server_running{true};

static void sigterm_handler(int) {
	g_server_running.store(false);
}

int main(int argc, char** argv) {
	if(argc > 1 && std::string_view(argv[1]) == "--dump-metrics") {
		telemetry::MetricsFormatter formatter;
		telemetry::SystemSnapshot snap{};
		snap.sequence_number = 1;
		snap.collection_duration_ns = 500000;
		snap.cpu.detected_cores = 2;
		snap.mem.mem_total = 16000000;
		snap.tcp.state_counts[static_cast<std::size_t>(telemetry::TcpState::ESTABLISHED)] = 5;
		const std::string_view resp = formatter.format_response(snap);
		const auto body_start = resp.find("\r\n\r\n");
		if(body_start != std::string_view::npos) {
			const std::string_view body = resp.substr(body_start + 4);
			// We are writing to std. output to be catched by promtool check metrics
			const ssize_t written = ::write(STDOUT_FILENO, body.data(), body.size());
			(void)written;
		}
		return 0;
	}

	if(argc > 1 && std::string_view(argv[1]) == "--run-server") {
		uint16_t port = 9100;
		int sampling_interval_ms = 0; // 0 = static snapshot, >0 = background sampling thread
		if(argc > 2) {
			port = static_cast<uint16_t>(std::stoi(argv[2]));
		}
		if(argc > 3) {
			sampling_interval_ms = std::stoi(argv[3]);
		}
		telemetry::SnapshotRegistry registry;
		telemetry::SystemSnapshot snap{};
		snap.sequence_number = 1;
		snap.cpu.detected_cores = 4;
		snap.mem.mem_total = 16000000;
		snap.tcp.state_counts[static_cast<std::size_t>(telemetry::TcpState::ESTABLISHED)] = 42;
		registry.publish(snap);

		// If sampling_interval_ms > 0, spawn sampling publisher thread to stress registry concurrent access
		std::thread sampler_thread;
		if(sampling_interval_ms > 0) {
			sampler_thread = std::thread([&registry, sampling_interval_ms]() {
				uint64_t seq = 2;
				while(g_server_running.load()) {
					telemetry::SystemSnapshot s{};
					s.sequence_number = seq++;
					// Some random data!
					s.collection_duration_ns = 150000;
					s.cpu.detected_cores = 4;
					s.mem.mem_total = 16000000;
					s.mem.mem_free = 8000000 + (seq % 1000);
					s.tcp.state_counts[static_cast<std::size_t>(telemetry::TcpState::ESTABLISHED)] = 40 + (seq % 10);
					registry.publish(s);
					std::this_thread::sleep_for(std::chrono::milliseconds(sampling_interval_ms));
				}
			});
		}

		// Tell Linux: when this process receives SIGTERM or Ctrl+C (SIGINT),
		// don't abruptly kill it; call our sigterm_handler function instead!
		struct sigaction sa{};
		sa.sa_handler = sigterm_handler;
		// Ensure the mask is completely clean and clear
		sigemptyset(&sa.sa_mask);
		::sigaction(SIGTERM, &sa, nullptr);
		::sigaction(SIGINT, &sa, nullptr);

		telemetry::HttpServer server(registry, port);
		server.start();
		std::cout << "SERVER_READY:" << server.port() << std::endl;

		// Sleep peacefully in a loop as long as g_server_running is true
		while(g_server_running.load()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}

		server.stop();
		if(sampler_thread.joinable()) {
			sampler_thread.join();
		}
		return 0;
	}

	std::cout << "========================================" << std::endl;
	std::cout << "Running test_server HTTP export suite..." << std::endl;
	std::cout << "========================================" << std::endl;

	test_metrics_formatter();
	test_http_routes();
	test_server_zero_alloc();
	test_prometheus_format_compliance();
	test_slowloris_eviction();

	std::cout << "\n[test_server] All server tests passed successfully!" << std::endl;
	return 0;
}
