# telemetryd

A **Zero-Allocation Linux Host Telemetry Daemon in Modern C++17**, parsing direct `/proc` kernel metrics and serving them via a non-blocking OpenMetrics (Prometheus-compatible) scraper endpoint.

---

## Architecture & Design Highlights

- **Zero Dynamic Allocations in Steady State**: Strict guarantee of zero heap allocations (`malloc`, `operator new`) during metric parsing and scraping cycles.
- **Direct `/proc` Parsing**: Zero-copy parsing directly over cache-aligned fixed scratch buffers (`ScratchBuffer<N>`).
- **Persistent File Descriptors**: Opens `/proc/stat`, `/proc/meminfo`, `/proc/net/tcp`, and `/proc/net/tcp6` once at startup with `O_RDONLY | O_CLOEXEC` to eliminate VFS path walk and inode lookup overhead.
- **TCP Socket Streaming & Line Boundary Splicing**: Streams unbounded socket tables from `/proc/net/tcp` and `/proc/net/tcp6` in 64 KiB chunks with in-place line boundary splicing (`std::memmove`) to aggregate connection state histograms and kernel queue pressures with zero heap allocations.
- **Double-Buffered State Machine (`SnapshotRegistry`)**: Two-slot lock-free snapshot registry (`alignas(64) std::array<SystemSnapshot, 2>`) with acquire-release barriers and active reader tracking for $O(1)$ lock-free read access without write contention.
- **Tear-Free Invariant Guarantee**: Readers acquire complete, internally consistent snapshots without torn reads or half-updated payload states, verified under high-contention test harnesses.
- **Absolute Deadline Clock Discipline (`SamplingEngine`)**: Collection loop scheduled via `std::chrono::steady_clock` with absolute monotonic deadlines (`sleep_until(next_tick)`) to eliminate cumulative clock drift over extended runtimes.
- **Error Containment & Fallback**: Transient `/proc` read errors are contained with zero-allocation logging via raw POSIX `::write(STDERR_FILENO, ...)` and last-known-good metric preservation.
- **Fast Numeric Parsing (`std::from_chars`)**: In-place ASCII parsing without string allocations, regex, or locale mutex contention.
- **Mechanical Allocation Enforcement**: Global C++ memory interceptors (`alloc_hook`) and an RAII `AllocGuard` harness that catches and aborts on any rogue heap allocations.
- **Defensive Systems Engineering**: Strict compiler flags (`-Wall -Wextra -Wpedantic -Wconversion -Werror`), cache-line alignment (`alignas(64)`), core bounds truncation, oversized record guards, and sanitizer validation (ASan, UBSan, TSan).
- **Link-Time Optimization (LTO)**: Interprocedural Optimization (IPO) enabled for release builds to permit cross-translation-unit inlining of hot parser routines.

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
