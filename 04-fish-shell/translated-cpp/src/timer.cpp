/// Timer snapshot implementation.
/// Transpiled from: fish-shell src/timer.rs (tag 4.0.0)
///
/// Key translation decisions:
///   Rust timeval_to_duration -> micros() helper (timeval -> int64_t microseconds)
///   Rust Duration::max(Duration::ZERO) -> std::max(0LL, value)
///   Rust format!() with named args -> snprintf into buffer
///   Rust write_all(output.as_bytes()) -> fputs(output.c_str(), stderr)

#include "timer.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

// ---- Helpers ----

/// Convert a timeval to microseconds.
static int64_t micros(const struct timeval& t) {
    return static_cast<int64_t>(t.tv_sec) * 1'000'000 +
           static_cast<int64_t>(t.tv_usec);
}

/// Convert a chrono duration to microseconds.
template <typename D1, typename D2>
static int64_t micros(const std::chrono::duration<D1, D2>& d) {
    return std::chrono::duration_cast<std::chrono::microseconds>(d).count();
}

/// Time unit for display.
enum class Unit { Minutes, Seconds, Millis, Micros };

/// Select the appropriate unit for a given microsecond count.
static Unit unit_for_micros(int64_t us) {
    if (us > 900'000'000) return Unit::Minutes;
    if (us >= 999'995)    return Unit::Seconds;
    if (us >= 1000)       return Unit::Millis;
    return Unit::Micros;
}

static const char* unit_short_name(Unit u) {
    switch (u) {
        case Unit::Minutes: return "mins";
        case Unit::Seconds: return "secs";
        case Unit::Millis:  return "millis";
        case Unit::Micros:  return "micros";
    }
    return "";
}

static const char* unit_long_name(Unit u) {
    switch (u) {
        case Unit::Minutes: return "minutes";
        case Unit::Seconds: return "seconds";
        case Unit::Millis:  return "milliseconds";
        case Unit::Micros:  return "microseconds";
    }
    return "";
}

static double convert_micros(int64_t us, Unit u) {
    switch (u) {
        case Unit::Minutes: return us / 1.0E6 / 60.0;
        case Unit::Seconds: return us / 1.0E6;
        case Unit::Millis:  return us / 1.0E3;
        case Unit::Micros:  return us / 1.0;
    }
    return 0.0;
}

// ---- TimerSnapshot ----

TimerSnapshot TimerSnapshot::take() {
    TimerSnapshot s;
    getrusage(RUSAGE_SELF, &s.cpu_fish);
    getrusage(RUSAGE_CHILDREN, &s.cpu_children);
    s.wall = std::chrono::steady_clock::now();
    return s;
}

std::string TimerSnapshot::get_delta(const TimerSnapshot& t1,
                                     const TimerSnapshot& t2,
                                     bool verbose) {
    int64_t fish_sys = micros(t2.cpu_fish.ru_stime) - micros(t1.cpu_fish.ru_stime);
    int64_t fish_usr = micros(t2.cpu_fish.ru_utime) - micros(t1.cpu_fish.ru_utime);
    int64_t child_sys = micros(t2.cpu_children.ru_stime) - micros(t1.cpu_children.ru_stime);
    int64_t child_usr = micros(t2.cpu_children.ru_utime) - micros(t1.cpu_children.ru_utime);

    // Clamp to zero — getrusage results may be slightly stale.
    fish_sys  = std::max(int64_t{0}, fish_sys);
    fish_usr  = std::max(int64_t{0}, fish_usr);
    child_sys = std::max(int64_t{0}, child_sys);
    child_usr = std::max(int64_t{0}, child_usr);

    int64_t net_wall_micros = micros(t2.wall - t1.wall);
    int64_t net_sys_micros  = fish_sys + child_sys;
    int64_t net_usr_micros  = fish_usr + child_usr;

    Unit wall_unit = unit_for_micros(net_wall_micros);
    Unit cpu_unit  = unit_for_micros(std::max(net_sys_micros, net_usr_micros));

    double wall_time = convert_micros(net_wall_micros, wall_unit);
    double sys_time  = convert_micros(net_sys_micros, cpu_unit);
    double usr_time  = convert_micros(net_usr_micros, cpu_unit);

    char buf[512];
    std::string output;

    if (!verbose) {
        output += "\n_______________________________";
        std::snprintf(buf, sizeof buf, "\nExecuted in  %6.2f %s",
                      wall_time, unit_long_name(wall_unit));
        output += buf;
        std::snprintf(buf, sizeof buf, "\n   usr time  %6.2f %s",
                      usr_time, unit_long_name(cpu_unit));
        output += buf;
        std::snprintf(buf, sizeof buf, "\n   sys time  %6.2f %s",
                      sys_time, unit_long_name(cpu_unit));
        output += buf;
    } else {
        Unit fish_unit  = unit_for_micros(std::max(fish_sys, fish_usr));
        Unit child_unit = unit_for_micros(std::max(child_sys, child_usr));

        double fish_usr_time  = convert_micros(fish_usr, fish_unit);
        double fish_sys_time  = convert_micros(fish_sys, fish_unit);
        double child_usr_time = convert_micros(child_usr, child_unit);
        double child_sys_time = convert_micros(child_sys, child_unit);

        int col2_len = static_cast<int>(
            std::max(std::strlen(unit_short_name(wall_unit)),
                     std::strlen(unit_short_name(cpu_unit))));

        output += "\n________________________________________________________";
        std::snprintf(buf, sizeof buf,
                      "\nExecuted in  %6.2f %-*s    %-*s  external",
                      wall_time, col2_len, unit_short_name(wall_unit),
                      static_cast<int>(std::strlen(unit_short_name(fish_unit))) + 7,
                      "fish");
        output += buf;
        std::snprintf(buf, sizeof buf,
                      "\n   usr time  %6.2f %-*s  %6.2f %s  %6.2f %s",
                      usr_time, col2_len, unit_short_name(cpu_unit),
                      fish_usr_time, unit_short_name(fish_unit),
                      child_usr_time, unit_short_name(child_unit));
        output += buf;
        std::snprintf(buf, sizeof buf,
                      "\n   sys time  %6.2f %-*s  %6.2f %s  %6.2f %s",
                      sys_time, col2_len, unit_short_name(cpu_unit),
                      fish_sys_time, unit_short_name(fish_unit),
                      child_sys_time, unit_short_name(child_unit));
        output += buf;
    }
    output += "\n";
    return output;
}

// ---- TimerGuard ----

TimerGuard::~TimerGuard() {
    auto end = TimerSnapshot::take();
    auto output = TimerSnapshot::get_delta(start_, end, true);
    std::fputs(output.c_str(), stderr);
    std::fputc('\n', stderr);
}

// ---- push_timer ----

TimerGuard push_timer() {
    return TimerGuard(TimerSnapshot::take());
}
