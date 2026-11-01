#pragma once

#include <cstdint>
#include <cstddef>
#include <charconv>
#include <string_view>
#include <fcntl.h>
#include <unistd.h>
#include <system_error>
#include <cerrno>
#include <utility>
#include <telemetry/common.hpp>

namespace telemetry {

// ==============================================================================
// 3. /proc/meminfo Architecture & Memory Pressure Computation
// ==============================================================================

// Memory snapshot representing point-in-time memory metrics reported by /proc/meminfo.
// All size fields are stored in KiB (kilobytes = 1024 bytes), matching the kernel's native format.
struct MemSnapshot {
    uint64_t mem_total{0};      // Total usable physical RAM
    // mem_free => Literally empty vs. mem_available => That can actually be given to a new process
    uint64_t mem_free{0};       // Unallocated RAM completely free of any use
    uint64_t mem_available{0};  // [This is the real metric, not mem_free] Kernel estimate of how much memory is available for starting new apps without swapping
    // buffers simplified => Temp. workspace RAM used by Linux while communicating with storage devices
    uint64_t buffers{0};        // In-memory block device cache (raw disk blocks)
    uint64_t cached{0};         // In-memory file page cache
    uint64_t swap_total{0};     // Total swap space configured
    uint64_t swap_free{0};      // Unused swap space remaining
    uint64_t active{0};         // Memory accessed recently (unlikely to be reclaimed)
    uint64_t inactive{0};       // Memory not accessed recently (candidates for page-out/swap-out)
    uint64_t dirty{0};          // Memory waiting to be flushed to disk

    // Derived metric: Memory currently in active use.
    // Prefers modern MemAvailable (introduced in Linux 3.14).
    // Falls back to traditional formula if MemAvailable is unavailable (0).
    constexpr uint64_t used() const noexcept {
        if (mem_available > 0 && mem_total >= mem_available) {
            // Modern calculation: MemTotal - MemAvailable
            return mem_total - mem_available;
        }
        // Traditional fallback: MemTotal - MemFree - (Buffers + Cached)
        const uint64_t reclaimed = mem_free + buffers + cached;
        if (mem_total >= reclaimed) {
            return mem_total - reclaimed;
        }
        return 0;
    }

    // Derived metric: Active memory utilization percentage [0.0, 100.0]
    constexpr double utilization_percent() const noexcept {
        if (mem_total == 0) {
            return 0.0;
        }
        const uint64_t u = used();
        const double pct = (static_cast<double>(u) / static_cast<double>(mem_total)) * 100.0;
        if (pct < 0.0) return 0.0;
        if (pct > 100.0) return 100.0;
        return pct;
    }

    // Derived metric: Total swap space currently in use
    constexpr uint64_t swap_used() const noexcept {
        if (swap_total >= swap_free) {
            return swap_total - swap_free;
        }
        return 0;
    }
};

// ==============================================================================
// In-Place Key-Value Fast Scanner
// ==============================================================================

// Fast in-place key-value scanner for /proc/meminfo.
// Parses format: <Key>:\s+<Value>\s+kB\n
// Zero heap allocations: uses std::string_view key matching and std::from_chars.
inline bool parse_proc_meminfo(std::string_view buffer, MemSnapshot& snapshot) noexcept {
    LineCursor cursor(buffer);
    std::string_view line;
    uint32_t matched_count = 0;
    constexpr uint32_t TOTAL_KEYS = 10;

    // 2. Step through each line
    while (cursor.next_line(line)) {
        if (line.empty()) {
            continue;
        }

        // 3. Locate the colon ':' and extract the key prefix
        const auto colon_pos = line.find(':');
        if (colon_pos == std::string_view::npos) {
            continue;
        }

        // KEY
        const std::string_view key = line.substr(0, colon_pos);

        // 4.a Advance past ':' and subsequent whitespace
        const char* ptr = line.data() + colon_pos + 1;
        const char* const end = line.data() + line.size();

        // 4.b Advance past subsequent whitespace
        while (ptr < end && (*ptr == ' ' || *ptr == '\t')) {
            ++ptr;
        }
        if (ptr >= end) {
            continue;
        }

        // 5. Parse the numeric value using std::from_chars
        uint64_t value = 0;
        const auto [next_ptr, ec] = std::from_chars(ptr, end, value);
        if (ec != std::errc()) {
            continue;
        }

        // 6. Match the key using fast static string comparisons
        if (key == "MemTotal") {
            snapshot.mem_total = value;
            ++matched_count;
        } else if (key == "MemFree") {
            snapshot.mem_free = value;
            ++matched_count;
        } else if (key == "MemAvailable") {
            snapshot.mem_available = value;
            ++matched_count;
        } else if (key == "Buffers") {
            snapshot.buffers = value;
            ++matched_count;
        } else if (key == "Cached") {
            snapshot.cached = value;
            ++matched_count;
        } else if (key == "SwapTotal") {
            snapshot.swap_total = value;
            ++matched_count;
        } else if (key == "SwapFree") {
            snapshot.swap_free = value;
            ++matched_count;
        } else if (key == "Active") {
            snapshot.active = value;
            ++matched_count;
        } else if (key == "Inactive") {
            snapshot.inactive = value;
            ++matched_count;
        } else if (key == "Dirty") {
            snapshot.dirty = value;
            ++matched_count;
        }

        // Stop parsing early once all 10 required keys are saturated
        if (matched_count >= TOTAL_KEYS) {
            break;
        }
    }

    return true;
}

// ==============================================================================
// MemCollector: Concrete RAII Telemetry Collector
// ==============================================================================

class MemCollector {
public:
    // Buffer size 4 KiB as required: matches OS page size, covers ~1.5-3 KiB /proc/meminfo.
    static constexpr std::size_t BUFFER_SIZE = 4096;

    // Constructor: Opens persistent descriptor to path (default "/proc/meminfo") with O_RDONLY | O_CLOEXEC.
    // Pre-allocates fixed-size internal buffer as member variable so stack usage remains constant.
    explicit MemCollector(const char* path = "/proc/meminfo") {
        fd_ = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd_ < 0) {
            throw std::system_error(errno, std::generic_category(), "MemCollector: failed to open meminfo path");
        }
    }

    // Destructor: RAII lifecycle discipline, automatically closes descriptor
    ~MemCollector() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    // Non-copyable to prevent duplicate file descriptor ownership
    MemCollector(const MemCollector&) = delete;
    MemCollector& operator=(const MemCollector&) = delete;

    // Movable: transfers file descriptor ownership cleanly
    MemCollector(MemCollector&& other) noexcept
        : buffer_(other.buffer_),
          snapshot_(other.snapshot_),
          fd_(other.fd_) {
        other.fd_ = -1;
    }

    MemCollector& operator=(MemCollector&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) {
                ::close(fd_);
            }
            buffer_ = other.buffer_;
            snapshot_ = other.snapshot_;
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    // sample() Method:
    // - Marked noexcept.
    // - Takes no heap memory.
    // - Resets read offset to 0 (atomic positional pread).
    // - Reads into the internal std::array buffer_.
    // - Runs the parsing routines and populates internal snapshot structs.
    bool sample() noexcept {
        if (fd_ < 0) {
            return false;
        }

        // Atomic positional read starting at offset 0
        const ssize_t bytes_read = ::pread(fd_, buffer_.data.data(), buffer_.data.size() - 1, 0);
        if (bytes_read <= 0) {
            return false;
        }

        buffer_.size = static_cast<std::size_t>(bytes_read);
        buffer_.data[buffer_.size] = '\0'; // Always null-terminate

        return parse_proc_meminfo(buffer_.view(), snapshot_);
    }

    // Convenience alias for sample()
    bool collect() noexcept {
        return sample();
    }

    // Accessors: Non-allocating const getters returning references or scalar values
    const MemSnapshot& snapshot() const noexcept { return snapshot_; }
    int fd() const noexcept { return fd_; }

private:
    ScratchBuffer<BUFFER_SIZE> buffer_{};
    MemSnapshot snapshot_{};
    int fd_{-1};
};

} // namespace telemetry
