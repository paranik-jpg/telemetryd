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

## Current Status

### Milestone 1: Framework Foundation
- [x] Project scaffold and directory tree (`include/`, `src/`, `tests/`, `benchmarks/`).
- [x] CMake 3.20+ build system with explicit configuration presets (`debug`, `release`, `asan`, `tsan`).
- [x] Strict compiler warnings matrix and sanitizer presets.
- [x] Linker discipline (`-pthread` and LTO/IPO support check).
- [x] Global memory allocation interceptor (`tests/alloc_hook.hpp`, `tests/alloc_hook.cpp`).
- [x] Scoped RAII surveillance guard (`AllocGuard`).
- [x] Baseline zero-allocation utilities (`ScratchBuffer`, `trim_leading_whitespace`, `split_once`, `LineCursor`).
- [x] Test harness validation (`test_zero_alloc` passing under standard Debug and ASan/UBSan presets).

### Milestone 2: CPU & Memory Telemetry Core
- [x] **CPU Collector (`CpuCollector`, `CpuJiffies`)**:
  - Direct zero-copy `/proc/stat` parsing into pre-allocated 16 KiB member buffers.
  - Multi-core tracking (up to `MAX_CORES = 64`) with safe upper-bound truncation.
  - Delta utilization math: $\Delta\text{Active} / \Delta\text{Total} \times 100$ clamped to $[0.0, 100.0]$.
  - Zero-delta and counter wrap-around guards preventing division-by-zero crashes.
- [x] **Memory Collector (`MemCollector`, `MemSnapshot`)**:
  - In-place key-value scanner for `/proc/meminfo` into pre-allocated 4 KiB member buffers.
  - Fast static string matching with early exit upon saturating 10 target memory keys.
  - Modern memory pressure math (`MemTotal - MemAvailable`) with seamless fallback to traditional buffers/cached calculations.
  - Swap utilization tracking (`SwapTotal - SwapFree`).
- [x] **Unit Testing & Sanitizer Verification**:
  - `test_cpu`: 1,000 consecutive iterations under `AllocGuard` with 0 heap allocations, synthetic input validation, and core overflow tests.
  - `test_mem`: 1,000 consecutive iterations under `AllocGuard` with 0 heap allocations and exact synthetic metric validation.
  - Deterministic file descriptor closure verification (`fcntl(fd, F_GETFD) == -1 && errno == EBADF`).
  - 100% test pass rate under AddressSanitizer and UndefinedBehaviorSanitizer presets with zero leaks.

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
