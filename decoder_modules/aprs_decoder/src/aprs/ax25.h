#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cmath>
#include <cstring>

// ---------------------------------------------------------------------------
//  AX.25 + APRS frame parser
//
//  parseAX25()  : decode the AX.25 address field (source/dest/digis + SSIDs)
//                 and split off the control/PID/info fields.
//  parseAPRS()  : decode the APRS info field into a structured record:
//                 station/object name, lat/lon, course, speed, symbol, comment.
//
//  Supported APRS data formats:
//    - Uncompressed position  ( ! = / @ )         + course/speed extension
//    - Compressed position    (base-91)           + compressed course/speed
//    - Objects                ( ; )               + course/speed
//    - Items                  ( ) )
//    - MIC-E                  ( ` ' 0x1c 0x1d )    lat in dest addr, lon/spd/crs in info
//    - Status / Message / etc.: kept as info text, no position
// ---------------------------------------------------------------------------

namespace aprs {

    struct AX25Frame {
        bool        valid = false;
        std::string source;          // e.g. "F4JTV-9"
        std::string dest;            // e.g. "APRS" or MIC-E dest "T7PSYW"
        std::string destRaw;         // 6 raw dest chars (for MIC-E lat decode)
        std::vector<std::string> path; // digipeaters with '*' if repeated
        uint8_t     control = 0;
        uint8_t     pid = 0;
        std::string info;            // APRS information field
    };

    struct APRSRecord {
        std::string name;            // station callsign or object/item name
        bool        hasPosition = false;
        double      lat = 0.0;
        double      lon = 0.0;
        bool        hasSpeed = false;
        double      speedKnots = 0.0;
        bool        hasCourse = false;
        int         course = 0;
        bool        hasAltitude = false; double altitudeM = 0.0;  // metres
        char        symbolTable = '/';
        char        symbolCode = '>';
        std::string comment;         // free-form remainder / status text
        std::string typeDesc;        // human-readable APRS data type

        // ---- weather (APRS weather report) ---------------------------------
        // Raw APRS units are kept here (degF, mph, hundredths-inch, hPa); unit
        // conversion to metric is done by the module when it builds the JSON.
        bool        hasWeather = false;
        bool        hasTemp = false;     double tempF = 0.0;        // temperature (deg F)
        bool        hasHumidity = false; int    humidity = 0;       // %
        bool        hasWindDir = false;  int    windDir = 0;        // degrees
        bool        hasWindSpd = false;  double windSpdMph = 0.0;   // mph (sustained)
        bool        hasGust = false;     double gustMph = 0.0;      // mph (peak 5 min)
        bool        hasPressure = false; double pressureHpa = 0.0;  // hPa
        bool        hasRain1h = false;   double rain1hIn = 0.0;     // inches (last hour)
    };

    // ---- helpers -----------------------------------------------------------

    inline std::string trimSpaces(const std::string& s) {
        size_t a = s.find_first_not_of(' ');
        if (a == std::string::npos) { return ""; }
        size_t b = s.find_last_not_of(' ');
        return s.substr(a, b - a + 1);
    }

    // Extract the "/A=nnnnnn" altitude extension (6 digits, FEET) from the
    // comment if present, convert to metres, and remove it from the comment.
    // Applies to uncompressed/object/item/compressed reports (MIC-E and
    // compressed positions may also carry their own altitude, set beforehand;
    // the leading guard keeps that value).
    inline void extractAltitude(APRSRecord& r) {
        if (r.hasAltitude) { return; }
        const std::string tag = "/A=";
        size_t pos = r.comment.find(tag);
        if (pos == std::string::npos) { return; }
        size_t ds = pos + tag.size();
        if (ds + 6 > r.comment.size()) { return; }
        long feet = 0; int digits = 0; bool neg = false; size_t i = ds;
        if (r.comment[i] == '-') { neg = true; i++; }
        for (; i < ds + 6 && i < r.comment.size(); i++) {
            char c = r.comment[i];
            if (c < '0' || c > '9') { return; }   // not a valid /A= field
            feet = feet * 10 + (c - '0'); digits++;
        }
        if (digits == 0) { return; }
        r.hasAltitude = true;
        r.altitudeM = (neg ? -(double)feet : (double)feet) * 0.3048;
        // Strip "/A=nnnnnn" from the comment for a cleaner display.
        size_t end = ds + 6;
        std::string cleaned = r.comment.substr(0, pos) + r.comment.substr(end);
        r.comment = trimSpaces(cleaned);
    }

    // Read `width` characters of a fixed-width numeric weather field starting at
    // `pos`. Returns true and sets `out` only if all chars are digits (an
    // optional leading '-' is allowed). Placeholder fields such as "..." or
    // spaces are treated as "no value" (returns false) but the caller still
    // advances by the fixed width so parsing stays aligned.
    inline bool readWxNum(const std::string& s, int pos, int width, double& out) {
        if (pos < 0 || pos + width > (int)s.size()) { return false; }
        int start = pos;
        bool neg = false;
        if (width > 1 && s[pos] == '-') { neg = true; start = pos + 1; }
        long val = 0; int digits = 0;
        for (int i = start; i < pos + width; i++) {
            char c = s[i];
            if (c < '0' || c > '9') { return false; }   // placeholder / not a number
            val = val * 10 + (c - '0'); digits++;
        }
        if (digits == 0) { return false; }
        out = neg ? -(double)val : (double)val;
        return true;
    }

    // Parse a contiguous APRS weather data segment beginning at `start`.
    // Recognised fields: c (wind dir), s (wind speed mph), g (gust mph),
    // t (temp degF), r (rain last hour, 1/100 in), p (rain 24h), P (rain since
    // midnight), h (humidity %), b (pressure 1/10 hPa), plus L/l (luminosity)
    // and # (rain counter) which are consumed but ignored. Scanning stops at
    // the first byte that is not a known weather field (that tail is the
    // software-type/unit identifier or free comment).
    inline void parseWeatherSegment(const std::string& s, int start, APRSRecord& r) {
        int i = start;
        int n = (int)s.size();
        bool any = false;
        double v;
        while (i < n) {
            char id = s[i];
            switch (id) {
                case 'c': // wind direction (deg)
                    if (readWxNum(s, i + 1, 3, v)) { r.hasWindDir = true; r.windDir = (int)v; any = true; }
                    i += 4; break;
                case 's': // sustained wind speed (mph)  [also snowfall in some ctx]
                    if (readWxNum(s, i + 1, 3, v)) { r.hasWindSpd = true; r.windSpdMph = v; any = true; }
                    i += 4; break;
                case 'g': // gust (mph)
                    if (readWxNum(s, i + 1, 3, v)) { r.hasGust = true; r.gustMph = v; any = true; }
                    i += 4; break;
                case 't': // temperature (deg F), 3 chars, may be negative
                    if (readWxNum(s, i + 1, 3, v)) { r.hasTemp = true; r.tempF = v; any = true; }
                    i += 4; break;
                case 'r': // rain last hour (1/100 inch)
                    if (readWxNum(s, i + 1, 3, v)) { r.hasRain1h = true; r.rain1hIn = v / 100.0; any = true; }
                    i += 4; break;
                case 'p': // rain last 24h (1/100 inch) - consumed, not stored
                case 'P': // rain since midnight - consumed, not stored
                    i += 4; break;
                case 'h': // humidity (%), 2 digits, 00 == 100%
                    if (readWxNum(s, i + 1, 2, v)) { r.hasHumidity = true; r.humidity = ((int)v == 0) ? 100 : (int)v; any = true; }
                    i += 3; break;
                case 'b': // barometric pressure (1/10 hPa), 5 digits
                    if (readWxNum(s, i + 1, 5, v)) { r.hasPressure = true; r.pressureHpa = v / 10.0; any = true; }
                    i += 6; break;
                case 'L': case 'l': // luminosity (3 digits) - skip
                    i += 4; break;
                case '#': // raw rain counter (3 digits) - skip
                    i += 4; break;
                default:
                    // Not a weather field: this is the software/unit id or the
                    // start of a free-form comment. Keep the remainder.
                    {
                        std::string tail = trimSpaces(s.substr(i));
                        if (!tail.empty()) { r.comment = tail; }
                    }
                    if (any) { r.hasWeather = true; }
                    return;
            }
        }
        if (any) { r.hasWeather = true; }
    }

    // Parse a 7-byte AX.25 address into "CALL-SSID" (+ '*' if repeated).
    inline std::string decodeAddress(const uint8_t* a, bool* lastFlag,
                                     std::string* rawCall = nullptr,
                                     int* ssidOut = nullptr,
                                     bool* repeatedOut = nullptr) {
        std::string call;
        for (int i = 0; i < 6; i++) {
            char c = (char)(a[i] >> 1);
            call += c;
        }
        std::string raw = call; // before trimming (6 chars, for MIC-E)
        call = trimSpaces(call);
        int ssid = (a[6] >> 1) & 0x0F;
        bool repeated = (a[6] & 0x80) != 0;     // "has been repeated" (H) bit
        if (lastFlag)   { *lastFlag = (a[6] & 0x01) != 0; }
        if (rawCall)    { *rawCall = raw; }
        if (ssidOut)    { *ssidOut = ssid; }
        if (repeatedOut){ *repeatedOut = repeated; }
        std::string out = call;
        if (ssid != 0) { out += "-" + std::to_string(ssid); }
        return out;
    }

    inline bool parseAX25(const uint8_t* f, int len, AX25Frame& out) {
        if (len < 15) { return false; } // 14 addr + 1 ctrl minimum

        // --- address field: dest(7), source(7), 0..8 digis(7) ---
        int pos = 0;
        bool last = false;

        // Destination
        {
            std::string raw; int ssid; bool rep;
            out.dest = decodeAddress(f + pos, &last, &raw, &ssid, &rep);
            out.destRaw = raw;
            pos += 7;
        }
        if (last || pos + 7 > len) { return false; } // need source too

        // Source
        {
            out.source = decodeAddress(f + pos, &last);
            pos += 7;
        }

        // Digipeaters
        int guard = 0;
        while (!last && pos + 7 <= len && guard < 8) {
            std::string raw; int ssid; bool rep;
            std::string d = decodeAddress(f + pos, &last, &raw, &ssid, &rep);
            if (rep) { d += "*"; }
            out.path.push_back(d);
            pos += 7;
            guard++;
        }
        if (!last) { return false; } // address field never terminated

        // Control + PID
        if (pos >= len) { return false; }
        out.control = f[pos++];
        // UI frame (0x03) carries a PID byte; otherwise no APRS payload.
        if ((out.control & 0xEF) == 0x03) { // UI frame (poll bit masked)
            if (pos >= len) { return false; }
            out.pid = f[pos++];
        }

        // Information field
        if (pos <= len) {
            out.info.assign((const char*)(f + pos), (size_t)(len - pos));
        }
        out.valid = true;
        return true;
    }

    // ---- APRS info field decoding -----------------------------------------

    // Skip a leading APRS timestamp if present. Returns number of chars consumed.
    inline int skipTimestamp(const std::string& s, int start) {
        // DHM zulu "DDHHMMz", HMS "HHMMSSh", local "DDHHMM/"  -> 7 chars
        if ((int)s.size() >= start + 7) {
            char t = s[start + 6];
            bool digits = true;
            for (int i = 0; i < 6; i++) {
                if (!isdigit((unsigned char)s[start + i])) { digits = false; break; }
            }
            if (digits && (t == 'z' || t == 'h' || t == '/')) { return 7; }
        }
        return 0;
    }

    // Uncompressed: "DDMM.mmN/DDDMM.mmW$"  (lat=8, symtable=1, lon=9, symcode=1)
    inline bool parseUncompressed(const std::string& s, int start, APRSRecord& r,
                                  int* consumed) {
        if ((int)s.size() < start + 19) { return false; }
        std::string latStr = s.substr(start, 8);
        char symTable = s[start + 8];
        std::string lonStr = s.substr(start + 9, 9);
        char symCode = s[start + 18];

        // Latitude DDMM.mm + N/S (allow spaces for ambiguity)
        char ns = latStr[7];
        if (ns != 'N' && ns != 'S') { return false; }
        char ew = lonStr[8];
        if (ew != 'E' && ew != 'W') { return false; }

        auto num = [](const std::string& x) {
            std::string c; for (char ch : x) { c += (ch == ' ') ? '0' : ch; }
            return atof(c.c_str());
        };
        double latDeg = num(latStr.substr(0, 2));
        double latMin = num(latStr.substr(2, 5)); // "MM.mm"
        double lat = latDeg + latMin / 60.0;
        if (ns == 'S') { lat = -lat; }

        double lonDeg = num(lonStr.substr(0, 3));
        double lonMin = num(lonStr.substr(3, 5));
        double lon = lonDeg + lonMin / 60.0;
        if (ew == 'W') { lon = -lon; }

        r.hasPosition = true;
        r.lat = lat; r.lon = lon;
        r.symbolTable = symTable; r.symbolCode = symCode;
        *consumed = start + 19;

        // Course/speed extension: "CSE/SPD" -> "nnn/nnn" right after symbol
        int p = *consumed;
        if ((int)s.size() >= p + 7 && s[p + 3] == '/' &&
            isdigit((unsigned char)s[p]) && isdigit((unsigned char)s[p + 4])) {
            int crs = atoi(s.substr(p, 3).c_str());
            int spd = atoi(s.substr(p + 4, 3).c_str());
            r.hasCourse = true; r.course = crs;
            r.hasSpeed = true; r.speedKnots = spd; // already in knots
            *consumed = p + 7;
        }
        return true;
    }

    // Compressed position: symtable, 4 lat (b91), 4 lon (b91), symcode, 2 cs, comp type
    inline bool parseCompressed(const std::string& s, int start, APRSRecord& r,
                                int* consumed) {
        if ((int)s.size() < start + 13) { return false; }
        char symTable = s[start];
        auto b91 = [&](int off, int n) {
            long v = 0;
            for (int i = 0; i < n; i++) {
                int c = (unsigned char)s[start + off + i] - 33;
                if (c < 0 || c > 90) { return -1L; }
                v = v * 91 + c;
            }
            return v;
        };
        long y = b91(1, 4);
        long x = b91(5, 4);
        if (y < 0 || x < 0) { return false; }
        char symCode = s[start + 9];

        double lat = 90.0  - (double)y / 380926.0;
        double lon = -180.0 + (double)x / 190463.0;

        r.hasPosition = true;
        r.lat = lat; r.lon = lon;
        r.symbolTable = symTable; r.symbolCode = symCode;

        // cs bytes
        char c = s[start + 10];
        char sVal = s[start + 11];
        char compType = s[start + 12];
        if (c != ' ') {
            int ci = (unsigned char)c - 33;
            int si = (unsigned char)sVal - 33;
            int t  = (unsigned char)compType - 33;
            if (ci >= 0 && si >= 0) {
                if ((t & 0x18) == 0x10) {
                    // cs encodes altitude: alt(feet) = 1.002 ^ (ci*91 + si)
                    double altFeet = pow(1.002, (double)(ci * 91 + si));
                    r.hasAltitude = true; r.altitudeM = altFeet * 0.3048;
                } else if (ci <= 89) {
                    // cs encodes course/speed
                    int crs = ci * 4;
                    double spd = pow(1.08, (double)si) - 1.0; // knots
                    r.hasCourse = true; r.course = crs;
                    r.hasSpeed = true; r.speedKnots = spd;
                }
            }
        }
        *consumed = start + 13;
        return true;
    }

    // MIC-E: latitude in the 6-char destination, lon/speed/course in info bytes.
    inline bool parseMicE(const AX25Frame& fr, APRSRecord& r) {
        const std::string& d = fr.destRaw;       // 6 raw dest chars
        const std::string& info = fr.info;
        if (d.size() < 6 || info.size() < 9) { return false; }

        // Decode the 6 destination characters -> lat digits + flags.
        int latDigits[6];
        int nsFlag = 0;    // 0 = south, 1 = north
        int lonOffset = 0; // 0 or 100
        int ewFlag = 0;    // 0 = east, 1 = west  (we map to sign below)
        for (int i = 0; i < 6; i++) {
            char ch = d[i];
            int val;
            int msgBitNorthOffsetWest = 0; // for bytes 3,4,5
            if (ch >= '0' && ch <= '9')      { val = ch - '0'; msgBitNorthOffsetWest = 0; }
            else if (ch >= 'A' && ch <= 'J') { val = ch - 'A'; msgBitNorthOffsetWest = 1; }
            else if (ch == 'K')              { val = 0; msgBitNorthOffsetWest = 1; } // ambiguity
            else if (ch == 'L')              { val = 0; msgBitNorthOffsetWest = 0; }
            else if (ch >= 'P' && ch <= 'Y') { val = ch - 'P'; msgBitNorthOffsetWest = 1; }
            else if (ch == 'Z')              { val = 0; msgBitNorthOffsetWest = 1; }
            else { return false; }
            latDigits[i] = val;
            if (i == 3) { nsFlag    = msgBitNorthOffsetWest; }
            if (i == 4) { lonOffset = msgBitNorthOffsetWest ? 100 : 0; }
            if (i == 5) { ewFlag    = msgBitNorthOffsetWest; }
        }

        double latDeg = latDigits[0] * 10 + latDigits[1];
        double latMin = latDigits[2] * 10 + latDigits[3]
                      + (latDigits[4] * 10 + latDigits[5]) / 100.0;
        double lat = latDeg + latMin / 60.0;
        if (nsFlag == 0) { lat = -lat; } // 0 = south

        // Longitude from info bytes 1..3 (byte 0 is the data-type id).
        int d0 = (unsigned char)info[1] - 28;
        int m0 = (unsigned char)info[2] - 28;
        int h0 = (unsigned char)info[3] - 28;
        int lonDeg = d0 + lonOffset;
        if (lonDeg >= 180 && lonDeg <= 189) { lonDeg -= 80; }
        else if (lonDeg >= 190 && lonDeg <= 199) { lonDeg -= 190; }
        int lonMinInt = m0;
        if (lonMinInt >= 60) { lonMinInt -= 60; }
        double lonMin = lonMinInt + h0 / 100.0;
        double lon = lonDeg + lonMin / 60.0;
        if (ewFlag == 1) { lon = -lon; } // 1 = west

        // Speed & course from info bytes 4..6.
        int sp = (unsigned char)info[4] - 28;
        int dc = (unsigned char)info[5] - 28;
        int se = (unsigned char)info[6] - 28;
        int speed = sp * 10 + dc / 10;
        if (speed >= 800) { speed -= 800; }
        int course = (dc % 10) * 100 + se;
        if (course >= 400) { course -= 400; }

        r.hasPosition = true;
        r.lat = lat; r.lon = lon;
        r.hasSpeed = true;  r.speedKnots = speed;
        r.hasCourse = true; r.course = course;
        r.symbolCode  = info.size() > 7 ? info[7] : '>';
        r.symbolTable = info.size() > 8 ? info[8] : '/';
        // remainder (after symbol table id at byte 8) is comment / telemetry
        if (info.size() > 9) { r.comment = trimSpaces(info.substr(9)); }
        // MIC-E altitude: "XXX}" where the 3 chars before '}' are base-91 and
        // encode metres relative to a -10000 m datum.
        size_t brace = r.comment.find('}');
        if (brace != std::string::npos && brace >= 3) {
            int a0 = (unsigned char)r.comment[brace - 3] - 33;
            int a1 = (unsigned char)r.comment[brace - 2] - 33;
            int a2 = (unsigned char)r.comment[brace - 1] - 33;
            if (a0 >= 0 && a0 <= 90 && a1 >= 0 && a1 <= 90 && a2 >= 0 && a2 <= 90) {
                long val = (long)a0 * 91 * 91 + (long)a1 * 91 + a2;
                r.hasAltitude = true;
                r.altitudeM = (double)(val - 10000);
                // remove the "XXX}" token from the displayed comment
                r.comment.erase(brace - 3, 4);
                r.comment = trimSpaces(r.comment);
            }
        }
        // also honour an explicit /A= extension if present
        extractAltitude(r);
        r.typeDesc = "MIC-E";
        return true;
    }

    // After a position has been parsed (with `p` pointing just past the
    // position + course/speed extension), decide whether this is a weather
    // station. APRS encodes a weather station with the symbol code '_': in that
    // case the "nnn/nnn" course/speed slot actually carries wind direction and
    // wind speed (mph), and a weather data segment (gust/temp/rain/...) follows.
    // Otherwise the remainder is a normal free-form comment.
    inline void finishPositionComment(const std::string& info, int p, APRSRecord& r) {
        if (r.symbolCode == '_') {
            if (r.hasCourse) { r.hasWindDir = true; r.windDir = r.course; }
            if (r.hasSpeed)  { r.hasWindSpd = true; r.windSpdMph = r.speedKnots; }
            r.hasCourse = false; r.hasSpeed = false;   // not motion: it's wind
            r.hasWeather = true;
            if ((int)info.size() > p) { parseWeatherSegment(info, p, r); }
            r.typeDesc = "Weather";
        } else {
            if ((int)info.size() > p) { r.comment = trimSpaces(info.substr(p)); }
            extractAltitude(r);   // honour "/A=dddddd" altitude in the comment
        }
    }

    // Top-level APRS info parser. Returns a record; hasPosition tells whether
    // lat/lon were resolved (only positioned records are forwarded over TCP).
    inline APRSRecord parseAPRS(const AX25Frame& fr) {
        APRSRecord r;
        r.name = fr.source;          // default: the station callsign
        const std::string& info = fr.info;
        if (info.empty()) { r.typeDesc = "no-info"; return r; }

        char dti = info[0];
        switch (dti) {
            case '!': case '=': { // position, no timestamp
                r.typeDesc = "Position";
                int p = 1;
                if (parseUncompressed(info, p, r, &p) ||
                    parseCompressed(info, p, r, &p)) {
                    finishPositionComment(info, p, r);
                }
                break;
            }
            case '/': case '@': { // position with timestamp
                r.typeDesc = "Position+time";
                int p = 1 + skipTimestamp(info, 1);
                if (parseUncompressed(info, p, r, &p) ||
                    parseCompressed(info, p, r, &p)) {
                    finishPositionComment(info, p, r);
                }
                break;
            }
            case ';': { // object  ;NAME.....*timestamp...
                r.typeDesc = "Object";
                if ((int)info.size() >= 11) {
                    r.name = trimSpaces(info.substr(1, 9));
                    int p = 11; // skip ';' + 9-char name + live/kill flag
                    p += skipTimestamp(info, p);
                    if (parseUncompressed(info, p, r, &p) ||
                        parseCompressed(info, p, r, &p)) {
                        finishPositionComment(info, p, r);
                        if (r.typeDesc == "Weather") { r.typeDesc = "Weather (obj)"; }
                        else { r.typeDesc = "Object"; }
                    }
                }
                break;
            }
            case ')': { // item  )NAME!... (name 3..9 chars terminated by ! or _)
                r.typeDesc = "Item";
                size_t end = info.find_first_of("!_", 1);
                if (end != std::string::npos && end >= 2) {
                    r.name = trimSpaces(info.substr(1, end - 1));
                    int p = (int)end + 1;
                    if (parseUncompressed(info, p, r, &p) ||
                        parseCompressed(info, p, r, &p)) {
                        finishPositionComment(info, p, r);
                        if (r.typeDesc == "Weather") { r.typeDesc = "Weather (item)"; }
                        else { r.typeDesc = "Item"; }
                    }
                }
                break;
            }
            case '`': case '\'':           // MIC-E (current / old)
            case 0x1c: case 0x1d: {        // MIC-E (rev-0 beta forms)
                AX25Frame tmp = fr;
                if (!parseMicE(tmp, r)) { r.typeDesc = "MIC-E?"; }
                break;
            }
            case '>': { r.typeDesc = "Status";  r.comment = trimSpaces(info.substr(1)); break; }
            case ':': { r.typeDesc = "Message"; r.comment = trimSpaces(info.substr(1)); break; }
            case 'T': { r.typeDesc = "Telemetry"; r.comment = trimSpaces(info.substr(1)); break; }
            case '$': { r.typeDesc = "NMEA";    r.comment = trimSpaces(info.substr(1)); break; }
            case '_': { // positionless weather report: _MDHM + weather data
                r.typeDesc = "Weather";
                // "_" + 8-char timestamp (MMDDHHMM), then the weather segment.
                int p = 1;
                if ((int)info.size() >= 9) { p = 9; }
                parseWeatherSegment(info, p, r);
                r.hasWeather = true;          // it is a weather report by type
                break;
            }
            default:  { r.typeDesc = "Other";   r.comment = trimSpaces(info); break; }
        }
        return r;
    }

}
