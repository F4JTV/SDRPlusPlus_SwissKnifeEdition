// Standalone test for APRS altitude + course parsing (no SDR++ link required).
//   g++ -std=c++17 -O2 -I src test_altitude.cpp -o test_altitude && ./test_altitude
#include <cstdint>
#include <string>
#include <vector>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include "aprs/ax25.h"

using namespace aprs;

static AX25Frame mk(const std::string& src, const std::string& info,
                    const std::string& destRaw = "") {
    AX25Frame f; f.source = src; f.dest = "APRS"; f.destRaw = destRaw; f.info = info; return f;
}

static int failures = 0;
static void check(bool cond, const char* what) {
    printf("  %s %s\n", cond ? "OK:" : "FAIL:", what);
    if (!cond) { failures++; }
}

int main() {
    // 1) Uncompressed: course/speed extension + /A= altitude (10000 ft = 3048 m).
    {
        auto r = parseAPRS(mk("F4JTV-9", "=4318.00N/00522.00E>088/036/A=010000Test ADRASEC 06"));
        printf("[unc] crs=%d spd=%.0fkn alt=%.1fm cmt='%s'\n",
            r.course, r.speedKnots, r.altitudeM, r.comment.c_str());
        check(r.hasCourse && r.course == 88, "course 88 deg");
        check(r.hasSpeed && (int)r.speedKnots == 36, "speed 36 kn");
        check(r.hasAltitude && (int)(r.altitudeM + 0.5) == 3048, "altitude 10000 ft -> 3048 m");
        check(r.comment == "Test ADRASEC 06", "/A= token stripped from comment");
    }

    // 2) /A= altitude without course/speed (500 ft = 152.4 m).
    {
        auto r = parseAPRS(mk("F4JTV", "=4318.00N/00522.00E-/A=000500QRV"));
        printf("[altonly] crs=%d alt=%.1fm cmt='%s'\n", r.hasCourse, r.altitudeM, r.comment.c_str());
        check(!r.hasCourse, "no course");
        check(r.hasAltitude && (int)(r.altitudeM * 10 + 0.5) == 1524, "altitude 500 ft -> 152.4 m");
        check(r.comment == "QRV", "comment preserved");
    }

    // 3) Plain position without altitude -> hasAltitude false.
    {
        auto r = parseAPRS(mk("F4JTV", "=4318.00N/00522.00E>Hello"));
        printf("[noalt] alt=%d cmt='%s'\n", r.hasAltitude, r.comment.c_str());
        check(!r.hasAltitude, "no altitude reported");
    }

    printf("\nRESULT: %s\n", failures == 0 ? "PASS \xE2\x9C\x85" : "FAIL");
    return failures == 0 ? 0 : 1;
}
