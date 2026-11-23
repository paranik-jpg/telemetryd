# telemetryd

A **Zero-Allocation Linux Host Telemetry Daemon in Modern C++17**, parsing direct `/proc` kernel metrics and serving them via a non-blocking OpenMetrics (Prometheus-compatible) scraper endpoint.

---

## Architecture & Design Highlights

- **Zero Dynamic Allocations in Steady State**: Strict guarantee of zero heap allocations (`malloc`, `operator new`) during metric parsing and scraping cycles.
- **Direct `/proc` Parsing**: Zero-copy parsing directly over cache-aligned fixed scratch buffers (`ScratchBuffer<N>`).
- **Persistent File Descriptors**: Opens `/proc/stat`, `/proc/meminfo`, `/proc/net/tcp`, and `/proc/net/tcp6` once at startup with `O_RDONLY | O_CLOEXEC`, so it doesn't have to repeatedly search for them and open them again.
- **TCP Socket Streaming & Line Boundary Splicing**: Streams unbounded socket tables from `/proc/net/tcp` and `/proc/net/tcp6` in 64 KiB chunks with in-place line boundary splicing (`std::memmove`) to aggregate connection state histograms and kernel queue pressures with zero heap allocations.
- **Double-Buffered State Machine (`SnapshotRegistry`)**: Two-slot lock-free snapshot registry (`alignas(64) std::array<SystemSnapshot, 2>`) with acquire-release barriers and active reader tracking for $O(1)$ lock-free read access without write contention.
- **Tear-Free Invariant Guarantee**: Readers acquire complete, internally consistent snapshots without torn reads or half-updated payload states, verified under high-contention test harnesses.
- **Absolute Deadline Clock Discipline (`SamplingEngine`)**: Collection loop scheduled via `std::chrono::steady_clock` with absolute monotonic deadlines (`sleep_until(next_tick)`) to eliminate cumulative clock drift over extended runtimes.
- **Zero-Allocation OpenMetrics Formatter (`MetricsFormatter`)**: Pre-formats OpenMetrics (Prometheus-compatible) text representation directly into a 64-byte cache-aligned 128 KiB scratch buffer using fast `std::to_chars` with fixed 2-decimal precision. Generates full `# HELP`, `# TYPE`, metadata tags, dynamic `Content-Length` calculation, and strictly adheres to Prometheus naming specifications.
- **Embedded Dual-Stack Non-Blocking HTTP Server (`HttpServer`)**: Custom POSIX HTTP/1.1 engine bound to `AF_INET6` with dual-stack support (`IPV6_V6ONLY = 0`), `SO_REUSEADDR`, `SO_REUSEPORT`, and `TCP_NODELAY`.
- **Edge-Triggered epoll Event Loop**: Handles multiple client connections efficiently using non-blocking `epoll` and `accept4` for faster, scalable request handling.
- **Zero-Copy Routing & Slowloris Hardening**: Fixed 2 KiB stack read buffer with multi-chunk request buffering and 500ms connection timeouts (`SO_RCVTIMEO` / `SO_SNDTIMEO`). Stalled or malformed connections are evicted without deadlocking or exhausting descriptors.
- **Chunked Backpressure-Aware Transmission**: Handles non-blocking chunked writes with byte-offset tracking, managing full socket buffers (`EAGAIN` / `EWOULDBLOCK`) through dynamic `EPOLLOUT` registration and clean socket closure.
- **Signal-Safe Architecture**: Comprehensive `SIGPIPE` suppression at both process level (`signal(SIGPIPE, SIG_IGN)`) and socket transmission level (`MSG_NOSIGNAL`).
- **Error Containment & Fallback**: Transient `/proc` read errors are contained with zero-allocation logging via raw POSIX `::write(STDERR_FILENO, ...)` and last-known-good metric preservation.
- **Fast Numeric Parsing (`std::from_chars`)**: In-place ASCII parsing without string allocations, regex, or locale mutex contention.
- **Mechanical Allocation Enforcement**: Global C++ memory hooks and an RAII `AllocGuard` that detects and aborts unexpected heap allocations.
- **Defensive Systems Engineering**: Strict compiler flags (`-Wall -Wextra -Wpedantic -Wconversion -Werror`), cache-line alignment (`alignas(64)`), core bounds truncation and sanitizer validation (ASan, UBSan, TSan).
- **Link-Time Optimization (LTO)**: Interprocedural Optimization (IPO) enabled in release builds to allow inlining of frequently used parser functions across files.

---

## Metric Surface

The daemon exposes an OpenMetrics-compliant endpoint on `GET /metrics`:

| Metric Name | Type | Description |
| :--- | :--- | :--- |
| `host_cpu_utilization_ratio` | `gauge` | Aggregate CPU utilization ratio across user, system, iowait, irq, etc. |
| `host_cpu_core_utilization_ratio` | `gauge` | Per-core CPU utilization ratio labeled with `core="N"`. |
| `host_memory_total_bytes` | `gauge` | Total physical memory in bytes. |
| `host_memory_available_bytes` | `gauge` | Available memory in bytes from `/proc/meminfo`. |
| `host_memory_free_bytes` | `gauge` | Unused physical memory in bytes. |
| `host_memory_buffers_bytes` | `gauge` | Temporary storage for raw disk blocks in bytes. |
| `host_memory_cached_bytes` | `gauge` | In-memory cache for files read from disk in bytes. |
| `host_tcp_sockets_state_total` | `gauge` | Count of TCP sockets labeled by state (`established`, `syn_sent`, `time_wait`, `listen`, etc.). |
| `host_tcp_tx_queue_bytes` | `gauge` | Kernel TCP transmission buffer queue occupancy in bytes. |
| `host_tcp_rx_queue_bytes` | `gauge` | Kernel TCP receive buffer queue occupancy in bytes. |
| `host_scrape_duration_seconds` | `gauge` | Duration of the `/proc` telemetry collection cycle in seconds. |
| `host_scrape_sequence_number_total` | `counter` | Monotonically increasing scrape cycle sequence counter. |

---

## Building and Testing

### Prerequisites
- Linux Kernel 5.x+
- GCC 11+ or Clang 13+ (supporting ISO C++17)
- CMake 3.20+

### Debug Build & Run All Tests
```bash
cmake --preset debug
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

### ASan + UBSan Build & Run All Tests
```bash
cmake --preset asan
cmake --build build/asan
ctest --test-dir build/asan --output-on-failure
```

### ThreadSanitizer (TSan) Concurrency Test
```bash
cmake --preset tsan
cmake --build build/tsan
ctest --test-dir build/tsan --output-on-failure
```

### Optimized Release Build
```bash
cmake --preset release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```
