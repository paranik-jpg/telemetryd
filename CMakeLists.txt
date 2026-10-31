cmake_minimum_required(VERSION 3.20)
project(telemetryd VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
# Make the requested standard mandatory rather than a suggestion
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# A reusable "settings badge"
add_library(telemetryd_warnings INTERFACE)

# Attach the warning flags to the interface consumers.
# The INTERFACE visibility ensures that whoever links to telemetryd_warnings
# has these compile options applied to their translation units during compilation.
target_compile_options(telemetryd_warnings INTERFACE

    # Catches uninitialized variables, missing return statements in non-void functions,
    # out-of-bounds constant array indices, broken switch statements without breaks,
    # and suspicious syntax.
    -Wall

    # Detects comparisons between signed and unsigned integers,
    # empty body statements, unused function parameters,
    # missing field initializers in aggregate structs,
    # and sign-comparison bugs that commonly lead to buffer overflow vulnerabilities (Modern CPU issues).
    -Wextra

    # Forbids GNU-isms like variable-length arrays,
    # statement expressions (use lambdas and constexpr functions instead).
    -Wpedantic

    # Warns on implicit type conversions that may alter the numeric value or lose precision.
    # e.g. (cpp):-
    # uint64_t large_counter = 5'000'000'000ULL; // 5 billion (needs > 32 bits)
    # uint32_t small_counter = large_counter;    // Implicit narrowing!
    -Wconversion

    # Warns for implicit conversions that change the signedness of an integer.
    -Wsign-conversion

    # Warns whenever a local variable or parameter shadows another variable, parameter, or member in an outer scope.
    # Basically => Avoids naming new variables with alerady used names!
    -Wshadow

    # Warns when a class has virtual functions but lacks a virtual destructor.
    # To prevent leakage of derived class resources on deleting it through a base pointer!
    -Wnon-virtual-dtor

    # Forbids C-style casts like (int)x.
    # C-style casts are dangerous because they can silently perform
    # reinterpret_cast, const_cast, or static_cast without compiler distinction.
    -Wold-style-cast

    # Ensures pointers point to neat, aligned memory addresses.
    # e.g. => A 64-bit number wants to sit at a memory address divisible by 8.
    # If we cast an arbitrary byte offset to a uint64_t*,
    # it might land on an odd byte address. Modern CPUs hate this!!!
    -Wcast-align

    # Warns about unused stuff in our codebase.
    -Wunused

    # Warns when a derived class function doesn't override a virtual function of thes base class.
    # To avoid accidental declaration of new overload
    -Woverloaded-virtual

    # Enables maximum format string checks
    # Format string => template string that contains "placeholders" (specifiers like %d, %s, %f)
    -Wformat=2

    # Treats all warnings as fatal compilation errors.
    -Werror
)

# ==============================================================================
# Sanitizer Build Presets & Options
# ==============================================================================

# We can flip them on via the command line
option(ENABLE_ASAN "Enable AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan)" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer (TSan)" OFF)

# Enforce mutual exclusivity between ASan and TSan
if(ENABLE_ASAN AND ENABLE_TSAN)
    # Abort CMake configuration early!
    message(FATAL_ERROR "ENABLE_ASAN and ENABLE_TSAN are mutually exclusive. Cannot enable both simultaneously!")
endif()

# Reusable interface library for sanitizers
add_library(telemetryd_sanitizers INTERFACE)

# AddressSanitizer (ASan) hooks deeply into process memory
if(ENABLE_ASAN)
    message(STATUS "AddressSanitizer and UndefinedBehaviorSanitizer ENABLED")
    target_compile_options(telemetryd_sanitizers INTERFACE

        # address (ASan): Detects buffer overflows (reading past array boundaries).
        # undefined (UBSan): Detects undefined behavior, such as signed integer overflow, null-pointer dereferences, bitwise shifting beyond bit widths, or misaligned memory access.
        -fsanitize=address,undefined

        # Compilers enable omit frame pointer by default, but RBP registers are connected via Linked Lists and helps in tracing/debugging!
        # Forces the compiler to keep RBP (Frame Pointer) as the dedicated anchor register in every function.
        -fno-omit-frame-pointer

        # Attaches file names and line numbers to the binary
        -g
    )

    # Instructions given strictly to the linker!
    target_link_options(telemetryd_sanitizers INTERFACE
        -fsanitize=address,undefined
    )
# ThreadSanitizer (TSan) tracks every memory read/write instruction across CPU cores to find race conditions
elseif(ENABLE_TSAN)
    message(STATUS "ThreadSanitizer ENABLED")
    target_compile_options(telemetryd_sanitizers INTERFACE

        # Flags data races
        -fsanitize=thread
        -g
    )
    target_link_options(telemetryd_sanitizers INTERFACE
        -fsanitize=thread
    )
endif()

# ==============================================================================
# Linker Discipline: POSIX Threads & Interprocedural / Link-Time Optimization
# ==============================================================================

# 1. POSIX Threads (-pthread) on Linux
# Automatically detects the system threading model and creates the imported target Threads::Threads
find_package(Threads REQUIRED)

add_library(telemetryd_linker_discipline INTERFACE)

# Threads::Threads carries:
# Compile flags: -pthread and defines like -D_REENTRANT (which makes standard C library calls thread-safe).
# Link flags: -pthread (which tells the dynamic linker to link libpthread.so and wire up Linux thread-local storage / TLS structures).
target_link_libraries(telemetryd_linker_discipline INTERFACE Threads::Threads)

# 2. Link-Time Optimization (LTO / IPO) for Release builds
# Permits cross-translation-unit inlining of std::string_view parser utilities
# In short: Cross-translation-unit inlining gives clean, modular file architecture (separate .cpp files) with the blazing speed of a single monolithic header-only file.
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)

if(ipo_supported)
    message(STATUS "Link-Time Optimization (IPO/LTO) supported: ENABLED for Release builds")
else()
    message(WARNING "Link-Time Optimization (IPO/LTO) not supported: ${ipo_error}")
endif()

# ==============================================================================
# Common Include Directories & Subdirectories
# ==============================================================================

add_library(telemetryd_includes INTERFACE)

# "Whenever any target links to telemetryd_includes, tell the compiler to add -I/path/to/telemetryd/include to its build command."
target_include_directories(telemetryd_includes INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Unlock CMake’s built-in testing engine (CTest)
enable_testing()

# To execute the other two CMakeLists.txt
add_subdirectory(tests)
add_subdirectory(benchmarks)
