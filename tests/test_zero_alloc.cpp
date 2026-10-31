#include "alloc_hook.hpp"
#include <telemetry/common.hpp>

#include <cassert>
#include <iostream>
#include <vector>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// ==============================================================================
// Test Zero-Allocation Harness Verification
// ==============================================================================

static void test_alloc_accounting() {
    std::cout << "[RUN] test_alloc_accounting..." << std::endl;

    const std::size_t initial_allocs = g_allocation_count.load(std::memory_order_relaxed);
    const std::size_t initial_deallocs = g_deallocation_count.load(std::memory_order_relaxed);

    // Explicit heap allocation via dynamic array
    int* ptr = new int[100];
    assert(g_allocation_count.load(std::memory_order_relaxed) == initial_allocs + 1);

    delete[] ptr;
    assert(g_deallocation_count.load(std::memory_order_relaxed) == initial_deallocs + 1);

    std::cout << "  [PASS] test_alloc_accounting verified!" << std::endl;
}

static void test_zero_alloc_under_guard() {
    std::cout << "[RUN] test_zero_alloc_under_guard..." << std::endl;

    // Everything inside this scope is under strict zero-allocation surveillance
    {
        AllocGuard guard;

        // 1. Stack buffer allocation & view usage (0 heap allocations)
        telemetry::ScratchBuffer<1024> scratch;
        scratch.size = 14;
        for (std::size_t i = 0; i < scratch.size; ++i) {
            scratch.data[i] = 'A';
        }
        std::string_view sv = scratch.view();
        assert(sv.size() == 14);
        assert(sv[0] == 'A');

        // 2. Whitespace trimming (0 heap allocations)
        std::string_view padded = "   \t  test_val   ";
        std::string_view trimmed = telemetry::trim_leading_whitespace(padded);
        assert(trimmed.substr(0, 8) == "test_val");

        // 3. String splitting (0 heap allocations)
        std::string_view line = "cpu0:12345:67890";
        auto [head, tail] = telemetry::split_once(line, ':');
        assert(head == "cpu0");
        assert(tail == "12345:67890");

        // 4. Line cursor traversal (0 heap allocations)
        std::string_view multi_line = "first_line\nsecond_line\nthird_line\n";
        telemetry::LineCursor cursor(multi_line);
        std::string_view extracted;

        bool ok = cursor.next_line(extracted);
        assert(ok && extracted == "first_line");

        ok = cursor.next_line(extracted);
        assert(ok && extracted == "second_line");

        ok = cursor.next_line(extracted);
        assert(ok && extracted == "third_line");

        ok = cursor.next_line(extracted);
        assert(!ok && extracted.empty());

        // Mechanically verify that zero allocations occurred during this block
        guard.assert_zero_allocations();
    }

    std::cout << "  [PASS] test_zero_alloc_under_guard verified!" << std::endl;
}

// Test Case 2: Ensure that allocating a dynamic string inside AllocGuard triggers immediate trap / std::abort()
static void test_trap_on_illegal_allocation() {
    std::cout << "[RUN] test_trap_on_illegal_allocation (expecting child SIGABRT)..." << std::endl;

    // Sending a stunt double into the trap so the main actor doesn't get squashed.
    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        // Child process: intentionally violate the zero-allocation rule inside AllocGuard
        AllocGuard guard;

        // String far exceeding SSO limits (typically 15-22 bytes depending on standard library)
        // forces dynamic heap allocation via operator new
        std::string s = "A dynamically allocated string that exceeds small string optimization limits";
        (void)s;

        // If it somehow reached here, trap failed
        _exit(1);
    }

    int status = 0;
    // Suspend execution of the calling parent process until the child specified by pid has changed state.
    pid_t res = waitpid(pid, &status, 0);
    assert(res == pid);

    // True if termination is due to an unhandled signal, checks 'status'!
    assert(WIFSIGNALED(status));
    // Verify child was killed by signal SIGABRT, WTERMSIG converts:- status -> signal
    assert(WTERMSIG(status) == SIGABRT);

    std::cout << "  [PASS] test_trap_on_illegal_allocation verified! Child cleanly aborted with SIGABRT." << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Running test_zero_alloc harness..." << std::endl;
    std::cout << "========================================" << std::endl;

    test_alloc_accounting();
    test_zero_alloc_under_guard();
    test_trap_on_illegal_allocation();

    std::cout << "========================================" << std::endl;
    std::cout << "All test_zero_alloc checks passed successfully!" << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
