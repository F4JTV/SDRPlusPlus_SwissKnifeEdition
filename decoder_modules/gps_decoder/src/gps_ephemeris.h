// gps_ephemeris.h -- GPS broadcast ephemeris assembly
//
// One Ephemeris object holds everything needed to compute a satellite's ECEF
// position and apply its clock correction at any GPS time near the
// reference epoch (toe / toc). It is filled from three consecutive
// subframes (IDs 1, 2, 3) of the same broadcasting satellite.
//
// Field semantics, scaling factors and bit positions follow IS-GPS-200
// table 20-I and figures 20-1. The "semicircles" unit (π·rad) is converted
// to radians on parse, so consumers can use SI throughout.

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>

namespace gps {

// Raw subframe payload: 240 information bits, packed as 10 24-bit words
// (word[k] holds the 24 data bits of word k+1 in IS-GPS-200 numbering, in
// its low 24 bits, MSB = d1). The first 60 bits (preamble + TLM + HOW
// reserved bits + HOW) are NOT included here; only words 3..10 carry
// ephemeris content. words[0] and words[1] are filled but unused.
struct SubframeData {
    int prn         = 0;
    int subframeId  = 0;     // 1..5
    uint32_t words[10] = {}; // word[k].low24 = data bits of IS-GPS-200 word k+1
};

struct Ephemeris {
    int  prn = 0;
    bool sf1_received = false;
    bool sf2_received = false;
    bool sf3_received = false;

    // ---- Subframe 1 (clock corrections + URA) -----------------------------
    int    week        = 0;        // 10-bit GPS week (mod 1024); rollover handled externally
    int    code_on_L2  = 0;
    int    ura_index   = 0;        // 4-bit URA index
    int    sv_health   = 0;        // 6-bit health flag
    int    iodc        = 0;        // 10-bit, MSBs in sf1 word 3, LSBs in sf1 word 8
    double T_GD        = 0.0;      // group delay differential L1-L2 (s)
    double t_oc        = 0.0;      // clock data reference time (s into week)
    double a_f0        = 0.0;      // clock bias (s)
    double a_f1        = 0.0;      // clock drift (s/s)
    double a_f2        = 0.0;      // clock drift rate (s/s^2)

    // ---- Subframe 2 (ephemeris part 1) -----------------------------------
    int    iode_sf2    = 0;        // 8-bit ; must equal IODC mod 256
    double C_rs        = 0.0;      // (m)
    double dn          = 0.0;      // mean motion correction (rad/s)
    double M0          = 0.0;      // mean anomaly at reference time (rad)
    double C_uc        = 0.0;      // (rad)
    double e           = 0.0;      // eccentricity
    double C_us        = 0.0;      // (rad)
    double sqrtA       = 0.0;      // (m^1/2)
    double t_oe        = 0.0;      // ephemeris reference time (s into week)
    int    fit_flag    = 0;
    int    aodo        = 0;

    // ---- Subframe 3 (ephemeris part 2) -----------------------------------
    int    iode_sf3    = 0;        // 8-bit ; must equal iode_sf2
    double C_ic        = 0.0;      // (rad)
    double Omega0      = 0.0;      // longitude of ascending node (rad)
    double C_is        = 0.0;      // (rad)
    double i0          = 0.0;      // inclination at reference time (rad)
    double C_rc        = 0.0;      // (m)
    double omega       = 0.0;      // argument of perigee (rad)
    double Omega_dot   = 0.0;      // rate of right ascension (rad/s)
    double i_dot       = 0.0;      // rate of inclination (rad/s)

    // ---- Bookkeeping ------------------------------------------------------
    std::chrono::system_clock::time_point received_time;

    bool complete() const { return sf1_received && sf2_received && sf3_received; }

    // After all three subframes are in: confirm IODE-SF2, IODE-SF3 and the
    // 8 LSBs of IODC match (IS-GPS-200 §20.3.4.4 -- broadcasting satellite
    // guarantees this for a consistent ephemeris set).
    bool consistent() const {
        if (!complete()) return false;
        if (iode_sf2 != iode_sf3) return false;
        if ((iodc & 0xFF) != iode_sf2) return false;
        return true;
    }
};

// Decode bits 60..299 of a raw subframe into a SubframeData, applying the
// IS-GPS-200 parity check to words 3..10. `bits` points to 300 ±1-valued
// nav bits starting at the preamble of the subframe (bits[0..7] is the
// preamble). Returns false if any word fails parity.
//
// `d29_prev_word2` and `d30_prev_word2` are bits 29 and 30 of word 2 (HOW)
// of THIS subframe -- needed as D29*/D30* for word 3's parity check. The
// remaining words use the previous word's bits 29/30 (extracted internally).
//
// If `d29_prev_word2` == d30_prev_word2 == false the function will also
// try the alternate polarity (assume word 2 ended with ...01 or ...11)
// when the first check fails, which lets the very first subframe ever
// decoded bootstrap without needing the previous subframe's bits.
bool decodeSubframeWords(const int8_t* bits, int prn,
                         bool d29_prev_word2, bool d30_prev_word2,
                         SubframeData& out);

// Populate the corresponding fields of `eph` from a parity-checked
// subframe. Subframe ID must be 1, 2 or 3. Returns true if at least one
// field was updated.
bool applySubframeToEphemeris(const SubframeData& sf, Ephemeris& eph);

} // namespace gps
