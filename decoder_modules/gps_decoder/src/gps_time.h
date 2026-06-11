// gps_time.h -- GPS time → UTC conversion + system clock setting
//
// Given a TimeFix (a PC instant and the corresponding GPS time-of-week in
// seconds), compute the corresponding UTC instant for an arbitrary PC
// reference moment. Also provides a portable helper to apply a target UTC
// to the OS clock (requires admin/CAP_SYS_TIME on Linux, admin on Windows).
//
// Caveats:
//   - GPS week resolution uses the PC clock as a hint: as long as the PC is
//     within ±3.5 days of correct UTC, the week is recovered unambiguously.
//   - Leap seconds are NOT broadcast in the HOW word; they live in subframe
//     4 page 18 which arrives only every 12.5 minutes. We treat the leap
//     offset as a configurable parameter (default 18 s, valid since 2017-01).
//   - This module does NOT attempt to compensate for DSP pipeline latency
//     between the antenna and the bit-emission timestamp; expect O(10 ms)
//     constant bias which is still vastly better than NTP-over-LTE.

#pragma once

#include <chrono>
#include <string>

#include "gps_nav.h" // TimeFix

namespace gps {

// Number of leap seconds between GPS time and UTC (GPS is *ahead* of UTC).
// Since 2017-01-01 the value is 18. This file does not auto-decode it; if a
// new leap second is announced, bump the GUI knob or this constant.
inline constexpr int DEFAULT_LEAP_SECONDS = 18;

// Compute the UTC instant corresponding to a given PC instant, given a valid
// GPS time fix. The week number is recovered using `pc_now` as a hint, so
// the PC clock must be within ±3.5 days of correct UTC (which it essentially
// always is — a brand-new install has at minimum the BIOS clock or the
// install date).
//
// If `fix.valid` is false, returns `pc_now` unchanged.
std::chrono::system_clock::time_point gpsTowToUtc(
    const TimeFix& fix,
    std::chrono::system_clock::time_point pc_now,
    int leap_seconds = DEFAULT_LEAP_SECONDS);

// Format a system_clock::time_point as "YYYY-MM-DD HH:MM:SS.mmm UTC".
std::string formatUtcIso(std::chrono::system_clock::time_point tp);

// Attempt to set the OS real-time clock to `target` UTC. On success returns
// true with `errOut` empty. On failure (typically permission denied) returns
// false and writes a human-readable reason to `errOut`.
bool setSystemClock(std::chrono::system_clock::time_point target,
                    std::string& errOut);

} // namespace gps
