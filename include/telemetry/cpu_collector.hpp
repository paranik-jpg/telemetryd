#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <charconv>
#include <fcntl.h>
#include <unistd.h>
#include <system_error>
#include <cerrno>
#include <utility>
#include <telemetry/common.hpp>

namespace telemetry {

// ==============================================================================
// 2. /proc/stat Architecture & Metric Computation
// ==============================================================================

// Maximum number of individual CPU cores supported without heap allocations.
constexpr std::size_t MAX_CORES = 64;

// Cumulative counter jiffies since boot reported by the Linux kernel.
// (On standard Linux kernels, 1 jiffy is typically 10ms on 100 Hz / USER_HZ).
struct CpuJiffies {
    uint64_t user{0};        // Time spent in user space
    uint64_t nice{0};        // Time spent in user space with low priority (nice)
    uint64_t system{0};      // Time spent in kernel space (syscalls)
    uint64_t idle{0};        // Time spent idle
    uint64_t iowait{0};      // Time spent waiting for I/O to complete
    uint64_t irq{0};         // Time spent servicing hardware interrupts (Devices like NIC, an NVMe SSD controller, or a keyboard may trigger)
    uint64_t softirq{0};     // Time spent servicing software interrupts (The deferred, heavier follow-up work that was queued by a hard irq)
    uint64_t steal{0};       // Stolen time (time spent in other OS instances in a VM)
    uint64_t guest{0};       // Time spent running a virtual CPU for guest OS (VM-related)
    uint64_t guest_nice{0};  // Time spent running a niced guest virtual CPU (VM-related)

    // Total CPU jiffies across all states.
    // Note: guest and guest_nice are already included by the kernel in user and nice,
    // so they are not double-counted here.
    // constexp => Calculate beforehand!
    constexpr uint64_t total() const noexcept {
        return user + nice + system + idle + iowait + irq + softirq + steal;
    }

    // Active (working) CPU jiffies: Total minus idle and iowait.
    constexpr uint64_t active() const noexcept {
        const uint64_t idle_all = idle + iowait;
        const uint64_t tot = total();
        return (tot >= idle_all) ? (tot - idle_all) : 0;
    }
};

// ==============================================================================
// Snapshot State
// ==============================================================================

// Maintains previous and current jiffies across ticks to compute delta utilization rates.
// Uses fixed-size arrays: Index 0 is aggregate 'cpu', and indices 1..N are 'cpu0'..'cpuN'.
struct CpuCollectorState {
    std::array<CpuJiffies, MAX_CORES + 1> m_previous{};
    std::array<CpuJiffies, MAX_CORES + 1> m_current{};
    // If cores < MAX_CORES, we'll same traveral time in future!
    std::size_t detected_cores{0};
};

// Type alias: CpuSnapshot represents the state of all CPU cores across ticks
using CpuSnapshot = CpuCollectorState;

// ==============================================================================
// Delta Utilization Calculation
// ==============================================================================

// Computes CPU utilization percentage [0.0, 100.0] between two snapshot ticks.
// Strictly guards against division by zero, jiffy counter wrap-around, and tick jitter.
constexpr double compute_cpu_utilization(const CpuJiffies& prev, const CpuJiffies& curr) noexcept {
    const uint64_t total_prev = prev.total();
    const uint64_t total_curr = curr.total();

    // If total jiffies haven't advanced or wrapped around, report 0.0%
    if (total_curr <= total_prev) {
        return 0.0;
    }

    const uint64_t delta_total = total_curr - total_prev;
    const uint64_t active_prev = prev.active();
    const uint64_t active_curr = curr.active();

    if (active_curr <= active_prev) {
        return 0.0;
    }

    const uint64_t delta_active = active_curr - active_prev;

    // If delta_active >= delta_total due to non-atomic tick sampling jitter, clamp to 100.0%
    if (delta_active >= delta_total) {
        return 100.0;
    }

    // static_cast => When we know in-which type to cast during compile-time!
    const double usage = (static_cast<double>(delta_active) / static_cast<double>(delta_total)) * 100.0;

    // Defensive clamping to [0.0, 100.0]
    if (usage < 0.0) {
        return 0.0;
    }
    if (usage > 100.0) {
        return 100.0;
    }
    return usage;
}

// ==============================================================================
// In-Place Fast Parsing Algorithm (std::from_chars)
// ==============================================================================

// Helper: skips whitespace and parses the next uint64_t token via std::from_chars
// inline => Don't give error on multiple definitions!
// parse_next_jiffy(ptr to current reading pos, End of line ptr, output storage)
inline bool parse_next_jiffy(const char*& ptr, const char* end, uint64_t& value) noexcept {
    while (ptr < end && (*ptr == ' ' || *ptr == '\t')) {
        ++ptr;
    }
    if (ptr >= end) {
        return false;
    }
    // char seq. to num. value with zero heap allocation!
    // next_ptr points to first char that wasn't parsed!
    // from_chars stops strictly at any char which isn't digit!
    const auto [next_ptr, ec] = std::from_chars(ptr, end, value);
    // std::errc() => Success!
    if (ec != std::errc()) {
        return false;
    }
    ptr = next_ptr;
    return true;
}

// Fast in-place parser for /proc/stat contents.
// Performs 0 heap allocations, reads directly from std::string_view using std::from_chars.
inline bool parse_proc_stat(std::string_view buffer, CpuCollectorState& state) noexcept {
    LineCursor cursor(buffer);
    std::string_view line;

    while (cursor.next_line(line)) {
        if (line.empty()) {
            continue;
        }

        // Slot in m_current[...]
        std::size_t slot_index = 0;
        const char* ptr = line.data();
        const char* const end = line.data() + line.size();

        // 3. Check line prefix
        if (line.size() >= 4 && line[0] == 'c' && line[1] == 'p' && line[2] == 'u' && line[3] == ' ') {
            // "cpu " -> Aggregate core entry (Index 0)
            slot_index = 0;
            ptr += 4;
        } else if (line.size() >= 4 && line[0] == 'c' && line[1] == 'p' && line[2] == 'u' && (line[3] >= '0' && line[3] <= '9')) {
            // "cpuN" -> Individual core entry
            // Point to 'N'
            ptr += 3;
            uint32_t core_id = 0;
            const auto [next_ptr, ec] = std::from_chars(ptr, end, core_id);
            if (ec != std::errc()) {
                continue;
            }
            if (core_id >= MAX_CORES) {
                // Ignore cores beyond pre-allocated capacity
                continue;
            }
            // +1 => Because slot_index = 0 is for summary entry!
            slot_index = static_cast<std::size_t>(core_id) + 1;
            if (slot_index > state.detected_cores) {
                state.detected_cores = slot_index;
            }
            ptr = next_ptr;
        } else {
            // Prefix is not "cpu" (e.g. "intr", "ctxt", "btime", "processes")
            // Since CPU lines always appear at the top of /proc/stat, we can safely stop parsing!
            break;
        }

        // 4. Parse consecutive numeric fields using non-allocating whitespace-skipping token loop
        CpuJiffies jiffies{};
        if (!parse_next_jiffy(ptr, end, jiffies.user)) continue;
        if (!parse_next_jiffy(ptr, end, jiffies.nice)) continue;
        if (!parse_next_jiffy(ptr, end, jiffies.system)) continue;
        if (!parse_next_jiffy(ptr, end, jiffies.idle)) continue;

        // Optional kernel fields (older kernels had fewer fields, newer have up to 10)
        // (void) -> Discard function's return value.
        (void)parse_next_jiffy(ptr, end, jiffies.iowait);
        (void)parse_next_jiffy(ptr, end, jiffies.irq);
        (void)parse_next_jiffy(ptr, end, jiffies.softirq);
        (void)parse_next_jiffy(ptr, end, jiffies.steal);
        (void)parse_next_jiffy(ptr, end, jiffies.guest);
        (void)parse_next_jiffy(ptr, end, jiffies.guest_nice);

        // Store for the current CPU number (core_id)!
        state.m_current[slot_index] = jiffies;
    }

    return true;
}

// ==============================================================================
// CpuCollector: Concrete RAII Telemetry Collector
// ==============================================================================

class CpuCollector {
public:
    // Buffer size 16 KiB as required: accommodates systems up to 64-128 cores without reallocation.
    static constexpr std::size_t BUFFER_SIZE = 16384;

    // Constructor: Opens persistent descriptor to path (default "/proc/stat") with O_RDONLY | O_CLOEXEC.
    // Pre-allocates fixed-size internal buffer as member variable so stack usage remains constant.
    // explicit => "The compiler strictly bans automatic/implicit conversions"
    explicit CpuCollector(const char* path = "/proc/stat") {
        // O_RDONLY -> Read-only mode
        // O_CLOEXEC -> Prevent file descriptor leaks across fork()/execve()
        // :: explicity calls POSIX C open()
        fd_ = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd_ < 0) {
            // errno is thread-local, generic_category() formats human-readable messages
            throw std::system_error(errno, std::generic_category(), "CpuCollector: failed to open stat path");
        }
    }

    // Destructor: RAII lifecycle discipline, automatically closes descriptor
    ~CpuCollector() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    // Non-copyable to prevent duplicate file descriptor ownership
    CpuCollector(const CpuCollector&) = delete;
    CpuCollector& operator=(const CpuCollector&) = delete;

    // Movable: transfers file descriptor ownership cleanly
    // Move Constructor
    CpuCollector(CpuCollector&& other) noexcept
        : buffer_(other.buffer_),
          state_(other.state_),
          fd_(other.fd_) {
        // Remove the file holding from other!
        other.fd_ = -1;
    }

    // Move Assignment Operator
    CpuCollector& operator=(CpuCollector&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) {
                // Since an already existing CpuCollector is being overwritten
                ::close(fd_);
            }
            buffer_ = other.buffer_;
            state_ = other.state_;
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

        // Shift current snapshot to previous for delta rate computation
        state_.m_previous = state_.m_current;

        // Atomic positional read starting at offset 0
        const ssize_t bytes_read = ::pread(fd_, buffer_.data.data(), buffer_.data.size() - 1, 0);
        if (bytes_read <= 0) {
            return false;
        }

        buffer_.size = static_cast<std::size_t>(bytes_read);
        buffer_.data[buffer_.size] = '\0'; // Always null-terminate

        return parse_proc_stat(buffer_.view(), state_);
    }

    // Convenience alias for sample()
    bool collect() noexcept {
        return sample();
    }

    // Accessors: Non-allocating const getters returning references or scalar values
    const CpuSnapshot& snapshot() const noexcept { return state_; }
    const CpuCollectorState& state() const noexcept { return state_; }
    int fd() const noexcept { return fd_; }

private:
    ScratchBuffer<BUFFER_SIZE> buffer_{};
    CpuCollectorState state_{};
    int fd_{-1};
};

} // namespace telemetry
