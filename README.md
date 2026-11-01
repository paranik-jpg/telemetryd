# telemetryd

A **Zero-Allocation Linux Host Telemetry Daemon in Modern C++17**, parsing direct `/proc` kernel metrics and serving them via a non-blocking OpenMetrics (Prometheus-compatible) scraper endpoint.

---

## Architecture & Design Highlights

- **Zero Dynamic Allocations in Steady State**: Strict guarantee of zero heap allocations (`malloc`, `operator new`) during metric parsing and scraping cycles.
- **Direct `/proc` Parsing**: Zero-copy parsing directly over cache-aligned fixed scratch buffers (`ScratchBuffer<N>`).
- **Persistent File Descriptors**: Opens `/proc/stat` and `/proc/meminfo` once at startup with `O_RDONLY | O_CLOEXEC` and samples via atomic positional `pread(fd, ..., 0)` to eliminate VFS path walk and inode lookup overhead.
- **Fast Numeric Parsing (`std::from_chars`)**: In-place ASCII parsing without string allocations, regex, or locale mutex contention.
- **Mechanical Allocation Enforcement**: Global C++ memory interceptors (`alloc_hook`) and an RAII `AllocGuard` harness that catches and aborts on any rogue heap allocations.
- **Defensive Systems Engineering**: Strict compiler flags (`-Wall -Wextra -Wpedantic -Wconversion -Werror`), cache-line alignment (`alignas(64)`), core bounds truncation, and sanitizer validation (ASan, UBSan, TSan).
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

### Optimized Release Build
```bash
cmake --preset release
cmake --build build/release
```
