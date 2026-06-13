/*
 * Pure, side-effect-free helpers for parsing dumphfdl text output: block
 * classification, field extraction, and map-JSON construction. Kept in a
 * standalone header so the SDR++ module and the offline test harness share a
 * single source of truth.
 */
#pragma once
#include <string>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace hfdlparse {

enum MsgType { T_SQUITTER=0, T_LPDU, T_ACARS, T_POSITION, T_LOGON, T_DATA, T_OTHER, T_COUNT };

inline const char* typeName(int t) {
    static const char* n[T_COUNT] = {"Squitter","LPDU","ACARS","Position","Logon","Data","Other"};
    return (t >= 0 && t < T_COUNT) ? n[t] : "Other";
}

// A header line opens a new message block, e.g.
//   [2021-09-30 21:28:46 UTC] [11384.0 kHz] [24.7 Hz] [300 bps] [S]
inline bool isHeaderLine(const std::string& line) {
    return !line.empty() && line[0] == '[' && line.find("kHz]") != std::string::npos;
}

// Classify a block by the most specific HFDL layer/content it contains.
inline int classify(const std::string& b) {
    if (b.find("ACARS:") != std::string::npos)                    return T_ACARS;
    if (b.find("Lat:") != std::string::npos &&
        b.find("Lon:") != std::string::npos)                      return T_POSITION;
    if (b.find("Logon") != std::string::npos ||
        b.find("Logoff") != std::string::npos)                    return T_LOGON;
    if (b.find("Squitter") != std::string::npos ||
        b.find("SPDU") != std::string::npos)                      return T_SQUITTER;
    if (b.find("Performance data") != std::string::npos ||
        b.find("Frequency data") != std::string::npos)            return T_DATA;
    if (b.find("LPDU") != std::string::npos ||
        b.find("MPDU") != std::string::npos)                      return T_LPDU;
    return T_OTHER;
}

// Return the trimmed value following the first occurrence of `key` up to EOL.
inline std::string grabField(const std::string& b, const char* key) {
    size_t p = b.find(key);
    if (p == std::string::npos) { return ""; }
    p += std::strlen(key);
    size_t e = b.find('\n', p);
    std::string v = b.substr(p, (e == std::string::npos ? b.size() : e) - p);
    size_t s = v.find_first_not_of(" \t");
    size_t f = v.find_last_not_of(" \t\r");
    if (s == std::string::npos) { return ""; }
    return v.substr(s, f - s + 1);
}

inline std::string jsonEscape(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o;
}

// If `block` carries a valid aircraft position, build the map-JSON line and
// return true; otherwise return false and leave `out` untouched. Schema mirrors
// the ADS-B/AIS modules: name, icao, date, time, lat, lon, type, speed, info.
inline bool buildPositionJson(const std::string& block, std::string& out) {
    std::string latS = grabField(block, "Lat:");
    std::string lonS = grabField(block, "Lon:");
    if (latS.empty() || lonS.empty()) { return false; }
    double lat = 0, lon = 0;
    try { lat = std::stod(latS); lon = std::stod(lonS); }
    catch (...) { return false; }
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) { return false; }
    if (lat == 0.0 && lon == 0.0) { return false; }

    std::string flight = grabField(block, "Flight ID:");
    std::string icao   = grabField(block, "ICAO:");
    std::string reg    = grabField(block, "Reg:");
    std::string nameStr = !flight.empty() ? flight
                        : (!reg.empty() ? reg
                        : (!icao.empty() ? ("ICAO:" + icao) : std::string("HFDL")));

    char dbuf[16] = {0}, tbuf[16] = {0};
    bool haveTs = false;
    if (!block.empty() && block[0] == '[') {
        int Y, Mo, D, H, Mi, S;
        if (std::sscanf(block.c_str(), "[%d-%d-%d %d:%d:%d", &Y, &Mo, &D, &H, &Mi, &S) == 6) {
            std::snprintf(dbuf, sizeof(dbuf), "%04d-%02d-%02d", Y, Mo, D);
            std::snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d", H, Mi, S);
            haveTs = true;
        }
    }
    if (!haveTs) {
        std::time_t tt = std::time(nullptr);
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &tt);
#else
        gmtime_r(&tt, &utc);
#endif
        std::strftime(dbuf, sizeof(dbuf), "%Y-%m-%d", &utc);
        std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &utc);
    }

    std::string info;
    std::string gs = grabField(block, "Src GS:");
    if (gs.empty()) gs = grabField(block, "Dst GS:");
    if (!gs.empty()) { info += "gs=" + gs; }

    char buf[640];
    std::snprintf(buf, sizeof(buf),
        "{\"name\":\"%s\",\"icao\":\"%s\",\"date\":\"%s\",\"time\":\"%s\","
        "\"lat\":%.6f,\"lon\":%.6f,\"type\":\"HFDL\",\"speed\":null,\"info\":\"%s\"}",
        jsonEscape(nameStr).c_str(), jsonEscape(icao).c_str(), dbuf, tbuf,
        lat, lon, jsonEscape(info).c_str());
    out = buf;
    return true;
}

} // namespace hfdlparse
