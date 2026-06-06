// Validate MIC-E and compressed-position parsing against reference values.
#include "aprs/ax25.h"
#include <cstdio>
#include <cmath>
using namespace aprs;

int main() {
    int fails = 0;

    // --- MIC-E test (encode->decode round-trip with known values) -----------
    // Target: 43 18.00 N, 005 22.00 E, speed 36 kn, course 88 deg.
    // Dest "431X00": digits 4,3,1,8,0,0 -> 43 18.00 ; byte3 'X'=North,
    //   byte4 '0'=offset 0, byte5 '0'=East.
    {
        AX25Frame fr;
        fr.source = "F4JTV-9";
        fr.destRaw = "431X00";
        std::string s;
        s.push_back('`');               // MIC-E data type id (current)
        s.push_back((char)(5  + 28));   // lon deg = 5 (+ offset 0)
        s.push_back((char)(22 + 28));   // lon min = 22
        s.push_back((char)(0  + 28));   // lon hundredths = 00
        s.push_back((char)(3  + 28));   // sp -> 30 kn
        s.push_back((char)(60 + 28));   // dc -> +6 kn, course hundreds 0
        s.push_back((char)(88 + 28));   // se -> course units 88
        s.push_back('>');               // symbol code
        s.push_back('/');               // symbol table
        s += "ADRASEC 06 mobile";       // comment
        fr.info = s;

        APRSRecord r = parseAPRS(fr);
        printf("[MIC-E] dest=%s -> hasPos=%d lat=%.5f lon=%.5f spd=%.1f crs=%d type=%s comment='%s'\n",
               fr.destRaw.c_str(), r.hasPosition, r.lat, r.lon, r.speedKnots, r.course,
               r.typeDesc.c_str(), r.comment.c_str());
        double expLat = 43.0 + 18.0/60.0;
        double expLon = 5.0  + 22.0/60.0;
        if (!r.hasPosition
            || fabs(r.lat - expLat) > 1e-4
            || fabs(r.lon - expLon) > 1e-4
            || fabs(r.speedKnots - 36.0) > 0.5
            || r.course != 88) {
            printf("  FAIL: MIC-E round-trip mismatch (exp lat=%.5f lon=%.5f spd=36 crs=88)\n",
                   expLat, expLon); fails++;
        } else {
            printf("  OK: MIC-E lat/lon/speed/course round-trip correct\n");
        }
    }

    // --- Compressed position test -------------------------------------------
    // Compressed format: =/YYYYXXXX$cs T  with symtable '/'.
    // Use a self-consistent encode: pick lat=43.3, lon=5.3667 and round-trip.
    {
        double latIn = 43.3, lonIn = 5.3667;
        long y = (long)llround((90.0 - latIn) * 380926.0);
        long x = (long)llround((180.0 + lonIn) * 190463.0);
        auto enc91 = [](long v, char* out) {
            for (int i = 3; i >= 0; i--) { out[i] = (char)(v % 91 + 33); v /= 91; }
        };
        char yb[4], xb[4]; enc91(y, yb); enc91(x, xb);
        std::string info = "=";
        info += '/';                       // symbol table
        info.append(yb, 4); info.append(xb, 4);
        info += '>';                        // symbol code
        info += "  ";                       // cs = no data
        info += 'T';                        // comp type
        info += "Compressed test";

        AX25Frame fr; fr.source = "F4JTV-5"; fr.info = info;
        APRSRecord r = parseAPRS(fr);
        printf("[COMPRESSED] hasPos=%d lat=%.5f lon=%.5f type=%s comment='%s'\n",
               r.hasPosition, r.lat, r.lon, r.typeDesc.c_str(), r.comment.c_str());
        if (!r.hasPosition || fabs(r.lat - latIn) > 0.01 || fabs(r.lon - lonIn) > 0.01) {
            printf("  FAIL: round-trip lat/lon mismatch\n"); fails++;
        } else {
            printf("  OK: compressed lat/lon round-trips\n");
        }
    }

    printf("\nRESULT: %s\n", fails == 0 ? "PASS ✅" : "FAIL ❌");
    return fails ? 1 : 0;
}
