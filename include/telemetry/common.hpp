#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

namespace telemetry {

// ==============================================================================
// 4. Baseline Types & Fixed Capacity Utilities
// ==============================================================================

// Aligned static/stack byte buffer type with zero heap allocations.
// Capacity => compile-time constant of type std::size_t
template <std::size_t Capacity>
struct ScratchBuffer {
    // alignas(64) => the compiler guarantees the starting memory address of data is a multiple of 64.
    // Helps in Cache-Line Alignment & SIMD Vectorization
    alignas(64) std::array<char, Capacity> data;
    std::size_t size{0};

    // std::string_view => lightweight, zero-copy non-owning viewer
    std::string_view view() const noexcept {
        return std::string_view(data.data(), size);
    }
};

// ==============================================================================
// String Parsing Utilities (Zero-Allocation)
// ==============================================================================

// constexpr => the compiler can evaluate at compile time
// In-place whitespace skipper: strips leading spaces, tabs, and carriage returns
constexpr std::string_view trim_leading_whitespace(std::string_view sv) noexcept {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r')) {
        // Internally, just does => ptr -> ptr + 1, length -> length − 1
        sv.remove_prefix(1);
    }
    return sv;
}

// Delimiter extractor: splits a string_view into [head, tail] around the first occurrence of delim
// without creating any temporary string objects.
constexpr std::pair<std::string_view, std::string_view> split_once(std::string_view sv, char delim) noexcept {
    const auto pos = sv.find(delim);
    if (pos == std::string_view::npos) {
        return {sv, std::string_view{}};
    }
    return {sv.substr(0, pos), sv.substr(pos + 1)};
}

// Line tokenizing cursor: A lightweight non-allocating cursor that steps through
// a multi-line buffer line-by-line using std::string_view::find('\n').
struct LineCursor {
    // remaining => (a view over the unread portion of the buffer).
    std::string_view remaining;

    constexpr explicit LineCursor(std::string_view buffer) noexcept : remaining(buffer) {}

    // Advances to the next line. Returns true if a line was extracted, false when exhausted.
    // Handles trailing '\r' (CRLF) cleanly if present.
    constexpr bool next_line(std::string_view& line) noexcept {
        if (remaining.empty()) {
            line = {};
            return false;
        }
        const auto pos = remaining.find('\n');
        if (pos == std::string_view::npos) {
            line = remaining;
            remaining = {};
        } else {
            line = remaining.substr(0, pos);
            remaining.remove_prefix(pos + 1);
        }
        // Strip carriage return '\r' if present at the end of the line
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        return true;
    }
};

} // namespace telemetry
