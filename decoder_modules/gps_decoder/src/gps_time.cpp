// gps_time.cpp -- GPS → UTC conversion and system clock setting

#include "gps_time.h"

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <Windows.h>
#else
  #include <time.h>
#endif

namespace gps {

namespace {

// 1980-01-06 00:00:00 UTC, expressed as Unix seconds.
// (315964800 has been the canonical value since the C library's time_t was
//  defined relative to 1970-01-01 UTC, on all common platforms.)
constexpr std::time_t GPS_EPOCH_UNIX = 315964800;
constexpr double WEEK_SECONDS = 7.0 * 86400.0;

std::chrono::system_clock::time_point gpsEpoch() {
    return std::chrono::system_clock::from_time_t(GPS_EPOCH_UNIX);
}

} // namespace

std::chrono::system_clock::time_point gpsTowToUtc(
    const TimeFix& fix,
    std::chrono::system_clock::time_point pc_now,
    int leap_seconds)
{
    if (!fix.valid) return pc_now;

    // 1. Project the measured TOW forward to "now" using PC-clock elapsed.
    //    The GPS clock is rock-stable; the PC clock has small drift but is
    //    fine over the few seconds to minutes between fixes.
    double elapsed_sec =
        std::chrono::duration<double>(pc_now - fix.pc_time).count();
    double tow_at_now = fix.gps_tow_seconds + elapsed_sec;

    // 2. Wrap into [0, WEEK_SECONDS) so we have a plain time-of-week value.
    double tow_wrapped = std::fmod(tow_at_now, WEEK_SECONDS);
    if (tow_wrapped < 0) tow_wrapped += WEEK_SECONDS;

    // 3. Determine the GPS week number using the PC clock as a hint. The PC
    //    clock is converted to "what it thinks GPS time is" by adding leap
    //    seconds. Then we extract the week-of-epoch and the within-week
    //    seconds the PC believes we're at.
    double pc_gps_seconds =
        std::chrono::duration<double>(pc_now - gpsEpoch()).count()
        + (double)leap_seconds;

    double pc_week_f = std::floor(pc_gps_seconds / WEEK_SECONDS);
    int64_t pc_week  = (int64_t)pc_week_f;
    double pc_tow    = pc_gps_seconds - pc_week_f * WEEK_SECONDS;

    // 4. Reconcile: if the measured TOW is far from the PC's idea of TOW,
    //    we must be on the adjacent week. ±3.5 days of slack on the PC.
    double diff = tow_wrapped - pc_tow;
    int64_t week_correction = 0;
    if (diff >  WEEK_SECONDS * 0.5) week_correction = -1;
    if (diff < -WEEK_SECONDS * 0.5) week_correction = +1;
    int64_t true_week = pc_week + week_correction;

    // 5. Assemble final GPS time and subtract leap seconds to get UTC.
    double true_gps_seconds_since_epoch =
        (double)true_week * WEEK_SECONDS + tow_wrapped;
    double true_utc_seconds_since_gps_epoch =
        true_gps_seconds_since_epoch - (double)leap_seconds;

    // Convert to system_clock::time_point with nanosecond precision.
    auto ns = std::chrono::nanoseconds(
        (int64_t)std::llround(true_utc_seconds_since_gps_epoch * 1e9));
    return gpsEpoch() + ns;
}

std::string formatUtcIso(std::chrono::system_clock::time_point tp) {
    auto secs_tp = std::chrono::time_point_cast<std::chrono::seconds>(tp);
    auto millis  = std::chrono::duration_cast<std::chrono::milliseconds>(
                       tp - secs_tp).count();
    if (millis < 0) {           // rounding cusp safety
        secs_tp -= std::chrono::seconds(1);
        millis  += 1000;
    }
    std::time_t tt = std::chrono::system_clock::to_time_t(secs_tp);
    std::tm tm_{};
#ifdef _WIN32
    gmtime_s(&tm_, &tt);
#else
    gmtime_r(&tt, &tm_);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  "%04d-%02d-%02d %02d:%02d:%02d.%03d UTC",
                  tm_.tm_year + 1900, tm_.tm_mon + 1, tm_.tm_mday,
                  tm_.tm_hour, tm_.tm_min, tm_.tm_sec, (int)millis);
    return buf;
}

bool setSystemClock(std::chrono::system_clock::time_point target,
                    std::string& errOut)
{
    auto since_epoch = target.time_since_epoch();
    auto secs  = std::chrono::duration_cast<std::chrono::seconds>(since_epoch);
    auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                     since_epoch - secs);

#ifdef _WIN32
    // SetSystemTime takes a SYSTEMTIME in UTC (year, month, day, ...).
    std::time_t tt = (std::time_t)secs.count();
    std::tm tm_{};
    if (gmtime_s(&tm_, &tt) != 0) {
        errOut = "gmtime_s failed";
        return false;
    }
    SYSTEMTIME st{};
    st.wYear         = (WORD)(tm_.tm_year + 1900);
    st.wMonth        = (WORD)(tm_.tm_mon + 1);
    st.wDayOfWeek    = (WORD)tm_.tm_wday;
    st.wDay          = (WORD)tm_.tm_mday;
    st.wHour         = (WORD)tm_.tm_hour;
    st.wMinute       = (WORD)tm_.tm_min;
    st.wSecond       = (WORD)tm_.tm_sec;
    st.wMilliseconds = (WORD)(nanos.count() / 1000000);
    if (!SetSystemTime(&st)) {
        DWORD e = GetLastError();
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "SetSystemTime failed (error %lu). "
                      "Run SDR++ as administrator.",
                      (unsigned long)e);
        errOut = msg;
        return false;
    }
    errOut.clear();
    return true;
#else
    struct timespec ts;
    ts.tv_sec  = (time_t)secs.count();
    ts.tv_nsec = (long)nanos.count();
    if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
        int err = errno;
        char msg[256];
        if (err == EPERM) {
            std::snprintf(msg, sizeof(msg),
                          "Permission denied (EPERM). Either run SDR++ as "
                          "root (sudo) or grant the binary the CAP_SYS_TIME "
                          "capability:\n"
                          "  sudo setcap cap_sys_time+ep $(which sdrpp)");
        } else {
            std::snprintf(msg, sizeof(msg),
                          "clock_settime failed: %s", std::strerror(err));
        }
        errOut = msg;
        return false;
    }
    errOut.clear();
    return true;
#endif
}

} // namespace gps
