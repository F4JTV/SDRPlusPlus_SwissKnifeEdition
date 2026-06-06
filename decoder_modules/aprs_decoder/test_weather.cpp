// Standalone test for APRS weather parsing (no SDR++ link required).
//   g++ -std=c++17 -O2 -I src test_weather.cpp -o test_weather && ./test_weather
#include <cstdint>
#include <string>
#include <vector>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include "aprs/ax25.h"

using namespace aprs;

static AX25Frame mk(const std::string& src, const std::string& info) {
    AX25Frame f; f.source = src; f.dest = "APRS"; f.info = info; return f;
}

static int failures = 0;
static void check(bool cond, const char* what) {
    printf("  %s %s\n", cond ? "OK:" : "FAIL:", what);
    if (!cond) { failures++; }
}

int main() {
    // 1) Position + weather (symbol '_'); wind dir/speed in the nnn/nnn slot.
    {
        auto r = parseAPRS(mk("F4JTV",
            "@092345z4903.50N/07201.75W_220/004g005t077r000p000P000h50b09900wRSW"));
        printf("[pos+wx] type=%s pos=%d wx=%d dir=%d spd=%.0fmph gust=%.0fmph t=%.0fF h=%d%% p=%.1fhPa cmt='%s'\n",
            r.typeDesc.c_str(), r.hasPosition, r.hasWeather, r.windDir, r.windSpdMph,
            r.gustMph, r.tempF, r.humidity, r.pressureHpa, r.comment.c_str());
        check(r.hasWeather, "flagged as weather");
        check(r.hasPosition, "has position");
        check(!r.hasSpeed, "speed slot reinterpreted as wind (no motion speed)");
        check(r.windDir == 220, "wind direction 220 deg");
        check((int)r.windSpdMph == 4, "wind speed 4 mph");
        check((int)r.gustMph == 5, "gust 5 mph");
        check((int)r.tempF == 77, "temperature 77 F");
        check(r.humidity == 50, "humidity 50%");
        check(r.pressureHpa > 989.9 && r.pressureHpa < 990.1, "pressure 990.0 hPa");
        check(r.comment == "wRSW", "software id captured as comment");
    }

    // 2) Positionless weather report: _MMDDHHMM + cNNN sNNN ...
    {
        auto r = parseAPRS(mk("F4JTV",
            "_10090556c220s004g005t077r000p000P000h50b09900wRSW"));
        printf("[posless] type=%s pos=%d wx=%d dir=%d spd=%.0fmph t=%.0fF h=%d%% p=%.1fhPa\n",
            r.typeDesc.c_str(), r.hasPosition, r.hasWeather, r.windDir, r.windSpdMph,
            r.tempF, r.humidity, r.pressureHpa);
        check(r.hasWeather, "flagged as weather");
        check(!r.hasPosition, "no position");
        check(r.windDir == 220, "wind direction 220 deg");
        check((int)r.tempF == 77, "temperature 77 F");
        check(r.humidity == 50, "humidity 50%");
    }

    // 3) Plain position must NOT be classified as weather.
    {
        auto r = parseAPRS(mk("F4JTV-9", "=4318.00N/00522.00E>088/036Test ADRASEC 06"));
        printf("[plain]  type=%s wx=%d spd=%.0fkn crs=%d cmt='%s'\n",
            r.typeDesc.c_str(), r.hasWeather, r.speedKnots, r.course, r.comment.c_str());
        check(!r.hasWeather, "not weather");
        check(r.hasSpeed && (int)r.speedKnots == 36, "normal speed preserved (36 kn)");
        check(r.comment == "Test ADRASEC 06", "comment preserved");
    }

    // 4) Negative temperature and 100% humidity (h00).
    {
        auto r = parseAPRS(mk("WX1", "=4903.50N/07201.75W_000/000g000t-05h00b10130"));
        printf("[neg t]  wx=%d t=%.0fF h=%d%% p=%.1fhPa\n",
            r.hasWeather, r.tempF, r.humidity, r.pressureHpa);
        check(r.hasWeather, "flagged as weather");
        check((int)r.tempF == -5, "temperature -5 F");
        check(r.humidity == 100, "h00 -> 100% humidity");
        check(r.pressureHpa > 1012.9 && r.pressureHpa < 1013.1, "pressure 1013.0 hPa");
    }

    printf("\nRESULT: %s\n", failures == 0 ? "PASS \xE2\x9C\x85" : "FAIL");
    return failures == 0 ? 0 : 1;
}
