# ==============================================================================
# Tests Subdirectory CMakeLists.txt
# ==============================================================================

# Memory allocation tracking hook library: telemetry_test_support
add_library(telemetry_test_support STATIC
    alloc_hook.cpp
    alloc_hook.hpp
)

# When compiling alloc_hook.cpp itself, search ${CMAKE_CURRENT_SOURCE_DIR} (tests/).
# And let any test file find "alloc_hook.hpp" easily.
target_include_directories(telemetry_test_support PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Equip telemetry_test_support with all our safety armor and compiler flags.
target_link_libraries(telemetry_test_support PUBLIC
    telemetryd_warnings
    telemetryd_sanitizers
    telemetryd_linker_discipline
    telemetryd_includes
)

# In test binaries, preserve assert() even under Release builds (-DNDEBUG)
# so test assertions actively validate conditions and prevent -Wunused warnings.
target_compile_options(telemetry_test_support PUBLIC
    -UNDEBUG
)

# ==============================================================================
# Zero-Allocation Verification Test Target
# ==============================================================================

add_executable(test_zero_alloc test_zero_alloc.cpp)
target_link_libraries(test_zero_alloc PRIVATE
    telemetry_test_support
)

# Registers this executable with CTest
add_test(NAME test_zero_alloc COMMAND test_zero_alloc)

# ==============================================================================
# CPU Collector Zero-Allocation Verification Test Target
# ==============================================================================

add_executable(test_cpu test_cpu.cpp)
target_link_libraries(test_cpu PRIVATE
    telemetry_test_support
)

add_test(NAME test_cpu COMMAND test_cpu)

# ==============================================================================
# Memory Collector Zero-Allocation Verification Test Target
# ==============================================================================

add_executable(test_mem test_mem.cpp)
target_link_libraries(test_mem PRIVATE
    telemetry_test_support
)

add_test(NAME test_mem COMMAND test_mem)

# ==============================================================================
# TCP Collector Zero-Allocation Verification Test Target
# ==============================================================================

add_executable(test_tcp test_tcp.cpp)
target_link_libraries(test_tcp PRIVATE
    telemetry_test_support
)

add_test(NAME test_tcp COMMAND test_tcp)

# ==============================================================================
# Snapshot Registry & Sampling Engine Concurrency Target
# ==============================================================================

add_executable(test_snapshot test_snapshot.cpp)
target_link_libraries(test_snapshot PRIVATE
    telemetry_test_support
)

add_test(NAME test_snapshot COMMAND test_snapshot)



