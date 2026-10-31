# telemetryd

A **Zero-Allocation Linux Host Telemetry Daemon in Modern C++17**, parsing direct `/proc` kernel metrics and serving them via a non-blocking OpenMetrics (Prometheus-compatible) scraper endpoint.

---

## Architecture & Design Highlights

- **Zero Dynamic Allocations in Steady State**: Strict guarantee of zero heap allocations (`malloc`, `operator new`) during metric parsing and scraping cycles.
- **Direct `/proc` Parsing**: Zero-copy parsing directly over aligned stack/fixed scratch buffers (`ScratchBuffer<N>`).
- **Mechanical Allocation Enforcement**: Global C++ memory interceptors (`alloc_hook`) and an RAII `AllocGuard` harness that catches and aborts on any rogue heap allocations.
- **Defensive Engineering**: Strict compiler flags (`-Wall -Wextra -Wpedantic -Wconversion -Werror`), cache-line alignment (`alignas(64)`), and sanitizers (ASan, UBSan, TSan).
- **Link-Time Optimization (LTO)**: Interprocedural Optimization (IPO) enabled for release builds to permit cross-translation-unit inlining of hot parser routines.

---

## Current Status (Milestone 1)

- [x] Project scaffold and directory tree (`include/`, `src/`, `tests/`, `benchmarks/`).
- [x] CMake 3.20+ build system with explicit configuration presets (`debug`, `release`, `asan`, `tsan`).
- [x] Strict compiler warnings matrix and sanitizer presets.
- [x] Linker discipline (`-pthread` and LTO/IPO support check).
- [x] Global memory allocation interceptor (`tests/alloc_hook.hpp`, `tests/alloc_hook.cpp`).
- [x] Scoped RAII surveillance guard (`AllocGuard`).
- [x] Baseline zero-allocation utilities (`ScratchBuffer`, `trim_leading_whitespace`, `split_once`, `LineCursor`).
- [x] Test harness validation (`test_zero_alloc` passing under standard Debug and ASan/UBSan presets).

---

## Building and Testing

### Prerequisites
- Linux Kernel 5.x+
- GCC 11+ or Clang 13+ (supporting ISO C++17)
- CMake 3.20+

### Debug Build & Run Tests
```bash
cmake --preset debug
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

### ASan + UBSan Build & Run Tests
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
