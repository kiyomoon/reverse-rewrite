/// Timer snapshot for measuring elapsed execution time.
/// Transpiled from: fish-shell src/timer.rs (tag 4.0.0)
///
/// Key translation decisions:
///   Rust Instant -> std::chrono::steady_clock::time_point
///   Rust Duration -> std::chrono::microseconds
///   Rust libc::rusage -> struct rusage (POSIX)
///   Rust enum Unit -> C++ enum class
///   Rust PrintElapsedOnDrop -> TimerGuard (RAII, prints in destructor)
///   Rust String formatting -> snprintf into std::string
///   Rust push_timer() -> push_timer()

#pragma once

#include <sys/resource.h>

#include <chrono>
#include <functional>
#include <string>

/// A snapshot of wall time and CPU usage.
struct TimerSnapshot {
    struct rusage cpu_fish;
    struct rusage cpu_children;
    std::chrono::steady_clock::time_point wall;

    /// Take a snapshot of the current state.
    static TimerSnapshot take();

    /// Format the delta between two snapshots.
    static std::string get_delta(const TimerSnapshot& t1,
                                 const TimerSnapshot& t2,
                                 bool verbose);

private:
    TimerSnapshot() = default;
};

/// RAII guard that prints elapsed time to stderr on destruction.
/// Translates Rust's PrintElapsedOnDrop.
class TimerGuard {
public:
    explicit TimerGuard(TimerSnapshot start) : start_(start) {}
    ~TimerGuard();

    TimerGuard(const TimerGuard&) = delete;
    TimerGuard& operator=(const TimerGuard&) = delete;
    TimerGuard(TimerGuard&&) = default;
    TimerGuard& operator=(TimerGuard&&) = default;

private:
    TimerSnapshot start_;
};

/// Create a timer snapshot and return a guard that prints elapsed time on drop.
TimerGuard push_timer();
