#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <system_error>
#include <telemetry/metrics_formatter.hpp>
#include <telemetry/snapshot.hpp>
#include <thread>

namespace telemetry {

// Static response constants stored in .rodata (0 dynamic allocations)
namespace http_responses {

inline constexpr std::string_view RESP_400 =
	"HTTP/1.1 400 Bad Request\r\n"
	"Content-Type: text/plain\r\n"
	"Content-Length: 15\r\n"
	"Connection: close\r\n"
	"\r\n"
	"400 Bad Request";

inline constexpr std::string_view RESP_404 =
	"HTTP/1.1 404 Not Found\r\n"
	"Content-Type: text/plain\r\n"
	"Content-Length: 13\r\n"
	"Connection: close\r\n"
	"\r\n"
	"404 Not Found";

inline constexpr std::string_view RESP_405 =
	"HTTP/1.1 405 Method Not Allowed\r\n"
	"Allow: GET\r\n"
	"Content-Type: text/plain\r\n"
	"Content-Length: 22\r\n"
	"Connection: close\r\n"
	"\r\n"
	"405 Method Not Allowed";

inline constexpr std::string_view RESP_414 =
	"HTTP/1.1 414 URI Too Long\r\n"
	"Content-Type: text/plain\r\n"
	"Content-Length: 16\r\n"
	"Connection: close\r\n"
	"\r\n"
	"414 URI Too Long";

inline constexpr std::string_view RESP_INDEX =
	"HTTP/1.1 200 OK\r\n"
	"Content-Type: text/html\r\n"
	"Content-Length: 76\r\n"
	"Connection: close\r\n"
	"\r\n"
	"<html><body><h1>telemetryd</h1><p><a href=\"/metrics\">Metrics</a></p></body></html>";

}  // namespace http_responses

// ==============================================================================
// HttpServer: Zero-Allocation Embedded POSIX Socket Prometheus Exporter
// ==============================================================================
class HttpServer {
   private:
	SnapshotRegistry& m_registry;
	uint16_t m_port{9100};
	int m_server_fd{-1};
	int m_epoll_fd{-1};

	std::atomic<bool> m_running{false};
	std::thread m_worker_thread{};

	// Reusable, aligned metric formatter owned by server instance
	MetricsFormatter m_formatter{};

	// Configure client socket: TCP_NODELAY, 500ms timeout ceilings
	// Returns false if any socket configuration fails
	static bool configure_client_socket(int client_fd) noexcept {
		// 1. Disable Nagle's algorithm
		int nodelay = 1;
		// IPPROTO_TCP => We are talking directly to the TCP layer (Other layers are generic socket layer AND internet protocol layer)
		if(::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0) {
			return false;
		}

		// 2. Strict 500ms timeout ceiling for receive and send
		struct timeval tv{};
		tv.tv_sec = 0;
		tv.tv_usec = 500'000;  // 500ms
		// SOL_SOCKET => General socket settings
		if(::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
			return false;
		}
		if(::setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
			return false;
		}

		return true;
	}

	// Chunked socket transmission loop with epoll backpressure handling
	bool send_all(int client_fd, std::string_view data) noexcept {
		std::size_t offset = 0;
		const std::size_t total_len = data.size();
		// buf is a constant pointer to constant characters
		const char* const buf = data.data();

		while(offset < total_len) {
			// When we send(client_fd, ...) or write(client_fd, ...) on a closed socket, the Linux kernel raises a SIGPIPE signal.
			// The default action for SIGPIPE in Linux is to kill your entire process immediately!
			// MSG_NOSIGNAL => Prevents that SIGPIPE generation
			const ssize_t sent = ::send(client_fd, buf + offset, total_len - offset, MSG_NOSIGNAL);

			if(sent > 0) {
				offset += static_cast<std::size_t>(sent);
				continue;
			}

			if(sent < 0) {
				if(errno == EINTR) {
					continue;
				}

				// Handle kernel buffer saturation
				// Instead of trying send() over and over
				if(errno == EAGAIN || errno == EWOULDBLOCK) {
					struct epoll_event ev{};
					// ev.events => Events we want to monitor on a fd.
					// EPOLLOUT => fd is ready to write.
					// EPOLLONESHOT => Disable monitoring after one event.
					ev.events = EPOLLOUT | EPOLLONESHOT;
					ev.data.fd = client_fd;
					// epoll_ctl => Manages fds monitored by an epoll instance
					// EPOLL_CTL_MOD => Modify the events being monitored for an already registered fd (here -> client_fd)
					if(::epoll_ctl(m_epoll_fd, EPOLL_CTL_MOD, client_fd, &ev) < 0) {
						// If socket wasn't previously added, try adding it
						if(::epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
							return false;
						}
					}

					struct epoll_event wait_ev{};
					const int nfds = ::epoll_wait(m_epoll_fd, &wait_ev, 1, 100);  // 100ms deadline
					if(nfds <= 0) {
						return false;  // Stalled client or timeout
					}
					continue;
				}

				// Ungraceful scraper disconnect
				if(errno == EPIPE || errno == ECONNRESET) {
					return false;  // [FALSE]: Client disconnected abruptly!
				}

				return false;  // [FALSE]: Unrecoverable socket error!
			}

			return false;  // [FALSE]: sent == 0 (connection closed by peer)
		}

		return true;  // [TRUE]: 100% of bytes flushed over the wire!
	}

	// Handles a single non-blocking client request with Slowloris protection
	void process_client(int client_fd) noexcept {
		// 1. Fixed-capacity 2 KiB stack read buffer
		std::array<char, 2048> request_buf{};
		std::size_t bytes_recv = 0;

		// Read until at least the HTTP request line delimiter ("\r\n") arrives,
		// or until the 500ms timeout ceiling expires (evicting Slowloris clients).
		while(bytes_recv < request_buf.size()) {
			struct pollfd pfd{};
			pfd.fd = client_fd;
			pfd.events = POLLIN;

			const int pr = ::poll(&pfd, 1, 500);  // Strict 500ms timeout ceiling
			if(pr <= 0) {
				// Timeout (500ms elapsed without data) or poll error -> evict stalled socket
				::close(client_fd);
				return;
			}

			const ssize_t n = ::recv(client_fd, request_buf.data() + bytes_recv, request_buf.size() - bytes_recv, MSG_DONTWAIT);
			if(n <= 0) {
				::close(client_fd);
				return;
			}

			bytes_recv += static_cast<std::size_t>(n);

			const std::string_view current_data(request_buf.data(), bytes_recv);
			if(current_data.find("\r\n") != std::string_view::npos) {
				break;	// Request line received
			}
		}

		// 2. Reject requests exceeding 2 KiB with 414 URI Too Long
		if(bytes_recv >= request_buf.size()) {
			send_all(client_fd, http_responses::RESP_414);
			::close(client_fd);
			return;
		}

		// 3. String-view path route check
		const std::string_view req(request_buf.data(), bytes_recv);
		const auto crlf_pos = req.find("\r\n");
		const std::string_view req_line = (crlf_pos != std::string_view::npos) ? req.substr(0, crlf_pos) : req;

		// Method check: must start with "GET "
		if(req_line.size() < 4 || req_line.substr(0, 4) != "GET ") {
			send_all(client_fd, http_responses::RESP_405);
			::close(client_fd);
			return;
		}

		// Extract target path
		const std::string_view rest = req_line.substr(4);  // Skip "GET "
		const auto space_pos = rest.find(' ');
		const std::string_view path = (space_pos != std::string_view::npos) ? rest.substr(0, space_pos) : rest;

		// Route matching
		if(path == "/metrics") {
			// Acquire active snapshot with zero lock contention
			const SystemSnapshot snap = m_registry.copy_active_snapshot();
			const std::string_view resp = m_formatter.format_response(snap);
			send_all(client_fd, resp);
		} else if(path == "/") {
			send_all(client_fd, http_responses::RESP_INDEX);
		} else {
			send_all(client_fd, http_responses::RESP_404);
		}

		// Socket Destruction: close client_fd once buffer is fully flushed
		::close(client_fd);
	}

	// Worker event loop driven by epoll
	void worker_loop() {
		constexpr int MAX_EVENTS = 64;
		std::array<struct epoll_event, MAX_EVENTS> events{};

		while(m_running.load(std::memory_order_relaxed)) {
			// epoll_wait with 50ms timeout to periodically check m_running flag
			const int nfds = ::epoll_wait(m_epoll_fd, events.data(), MAX_EVENTS, 50);

			for(int i = 0; i < nfds; ++i) {
				const auto idx = static_cast<std::size_t>(i);
				if(events[idx].data.fd == m_server_fd) {
					// Drain incoming connections via accept4() in a loop (EPOLLET edge-triggered)
					while(true) {
						struct sockaddr_in6 client_addr{};
						socklen_t client_len = sizeof(client_addr);

						const int client_fd = ::accept4(m_server_fd, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len, SOCK_NONBLOCK | SOCK_CLOEXEC);

						if(client_fd < 0) {
							if(errno == EAGAIN || errno == EWOULDBLOCK) {
								// There is nobody to serve. Stop calling numbers and sit down until the door bell rings again (epoll_wait).
								break;	// All pending connections accepted
							}
							if(errno == EINTR) {
								continue;
							}
							break;
						}

						// Configure client socket with TCP_NODELAY and 500ms timeout
						if(!configure_client_socket(client_fd)) {
							::close(client_fd);
							// continue => Next person in line, please!
							continue;
						}

						// Process client request non-blockingly
						process_client(client_fd);
					}
				}
			}
		}
	}

   public:
	explicit HttpServer(SnapshotRegistry& registry, uint16_t port = 9100) : m_registry(registry), m_port(port) {}

	~HttpServer() noexcept {
		stop();
	}

	// Non-copyable and non-movable
	HttpServer(const HttpServer&) = delete;
	HttpServer& operator=(const HttpServer&) = delete;
	HttpServer(HttpServer&&) = delete;
	HttpServer& operator=(HttpServer&&) = delete;

	// Start server: creates dual-stack socket, epoll instance, and worker thread
	void start() {
		if(m_running.exchange(true, std::memory_order_acq_rel)) {
			return;	 // Already running
		}

		// 1. Process-level SIGPIPE suppression
		// SIGPIPE is signal & SIG_IGN is its handler!
		if(::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
			m_running.store(false, std::memory_order_release);
			throw std::system_error(errno, std::generic_category(), "HttpServer: failed to set SIGPIPE to SIG_IGN");
		}

		// 2. Create dual-stack IPv4/IPv6 socket: AF_INET6, SOCK_NONBLOCK | SOCK_CLOEXEC
		// (Address Family: Internet IPv6, TCP | Non-Blocking Mode | Close-on-Exec for daddy issues, 0)
		m_server_fd = ::socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
		if(m_server_fd < 0) {
			m_running.store(false, std::memory_order_release);
			throw std::system_error(errno, std::generic_category(), "HttpServer: failed to create socket");
		}

		// 3. Socket Options: SO_REUSEADDR and SO_REUSEPORT
		int opt = 1;
		// SO_REUSEADDR => Escapes TIME_WAIT of previous connections
		if(::setsockopt(m_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
			::close(m_server_fd);
			m_server_fd = -1;
			m_running.store(false, std::memory_order_release);
			throw std::system_error(errno, std::generic_category(), "HttpServer: setsockopt(SO_REUSEADDR) failed");
		}
		// SO_REUSEPORT => Multiple sockets are sharing a port with incoming traffic distribution
		if(::setsockopt(m_server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
			::close(m_server_fd);
			m_server_fd = -1;
			m_running.store(false, std::memory_order_release);
			throw std::system_error(errno, std::generic_category(), "HttpServer: setsockopt(SO_REUSEPORT) failed");
		}

		// 4. Disable IPv6-only mode (IPV6_V6ONLY = 0) for dual-stack operation
		int v6only = 0;
		if(::setsockopt(m_server_fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) < 0) {
			::close(m_server_fd);
			m_server_fd = -1;
			m_running.store(false, std::memory_order_release);
			throw std::system_error(errno, std::generic_category(), "HttpServer: setsockopt(IPV6_V6ONLY=0) failed");
		}

		// 5. Bind to in6addr_any and configured port
		// Define the server address structure
		struct sockaddr_in6 server_addr{};
		server_addr.sin6_family = AF_INET6;
		// Listen on all network interfaces (localhost, Wi-Fi, etc.) -> deeper => I.P.
		server_addr.sin6_addr = in6addr_any;
		// Convert 8080 to Network Byte Order
		server_addr.sin6_port = htons(m_port);

		if(::bind(m_server_fd, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
			::close(m_server_fd);
			m_server_fd = -1;
			m_running.store(false, std::memory_order_release);
			throw std::system_error(errno, std::generic_category(), "HttpServer: bind failed");
		}

		// 6. Listen with generous backlog
		if(::listen(m_server_fd, 128) < 0) {
			::close(m_server_fd);
			m_server_fd = -1;
			m_running.store(false, std::memory_order_release);
			throw std::system_error(errno, std::generic_category(), "HttpServer: listen failed");
		}

		// 7. Epoll instance: epoll_create1(EPOLL_CLOEXEC)
		m_epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
		if(m_epoll_fd < 0) {
			::close(m_server_fd);
			m_server_fd = -1;
			m_running.store(false, std::memory_order_release);
			throw std::system_error(errno, std::generic_category(), "HttpServer: epoll_create1 failed");
		}

		// Register server socket with EPOLLIN | EPOLLET
		struct epoll_event ev{};
		// EPOLLIN => "Notify me when there is data ready to read."
		// EPOLLET => Don't buzz continuously, but once!
		ev.events = EPOLLIN | EPOLLET;
		ev.data.fd = m_server_fd;
		if(::epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, m_server_fd, &ev) < 0) {
			::close(m_epoll_fd);
			::close(m_server_fd);
			m_epoll_fd = -1;
			m_server_fd = -1;
			m_running.store(false, std::memory_order_release);
			throw std::system_error(errno, std::generic_category(), "HttpServer: epoll_ctl add server failed");
		}

		// 8. Launch worker thread
		// (What function to call, On which object to call);
		m_worker_thread = std::thread(&HttpServer::worker_loop, this);
	}

	// Stop server cleanly and reclaim all descriptors
	void stop() noexcept {
		if(!m_running.exchange(false, std::memory_order_acq_rel)) {
			return;	 // Already stopped
		}

		if(m_worker_thread.joinable()) {
			m_worker_thread.join();
		}

		if(m_epoll_fd >= 0) {
			::close(m_epoll_fd);
			m_epoll_fd = -1;
		}

		if(m_server_fd >= 0) {
			::close(m_server_fd);
			m_server_fd = -1;
		}
	}

	// Direct synchronous request handler for testing without full network loop
	void handle_connection(int client_fd) noexcept {
		if(configure_client_socket(client_fd)) {
			process_client(client_fd);
		} else {
			::close(client_fd);
		}
	}

	bool is_running() const noexcept {
		return m_running.load(std::memory_order_relaxed);
	}

	uint16_t port() const noexcept {
		return m_port;
	}
};

}  // namespace telemetry
