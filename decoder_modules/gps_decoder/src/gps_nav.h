// gps_nav.h -- 50 bps Navigation Message decoder
//
// The tracking channel emits one soft symbol every 1 ms (the sign of the
// prompt correlator). Twenty consecutive symbols of the same polarity make
// up a single navigation data bit (so the nav rate is 50 bps). The data
// stream is organised into:
//
//     1 frame    = 1500 bits   (= 30 seconds)
//     5 subframes / frame   (300 bits each = 6 s)
//     10 words   / subframe (30 bits each, 24 data + 6 parity)
//
// Each subframe starts with the 8-bit preamble 0x8B (10001011). After
// finding the preamble in the bit stream, the next 22 bits of the TLM word
// and the 30 bits of the HOW word can be parsed to extract:
//
//     - TOW (time of week, 17 bits in the HOW)
//     - Subframe ID (3 bits, values 1..5)
//
// This module only handles bit synchronisation, polarity ambiguity
// resolution, and subframe-level parsing. Ephemeris extraction (subframes
// 1-3) and almanac extraction (subframes 4-5) are not implemented here
// because they require Hamming(32,26) parity checks and a complete
// pseudorange/position solver to be useful -- see the README for pointers.

#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace gps {

// A time fix produced by a per-PRN NAV decoder. Together they say:
//   "at PC instant `pc_time`, the GPS TOW was `gps_tow_seconds`."
// The fix is captured at the moment the FIRST bit of a subframe's preamble
// was emitted by the bit synchroniser. The TOW value comes from the HOW word
// of the SAME subframe (the HOW gives the TOW count for the NEXT subframe, so
// we subtract one subframe = 6 s, with wraparound at end of week).
struct TimeFix {
    bool   valid             = false;
    int    prn               = 0;
    double gps_tow_seconds   = 0.0;
    std::chrono::system_clock::time_point pc_time;
    float  cn0_dBHz          = 0.0f; // copied in by the orchestrator
};

struct SubframeInfo {
    int      prn      = 0;
    int      subframeId = 0;  // 1..5
    uint32_t tow_count = 0;   // raw 17-bit field of HOW (Z-count of NEXT subframe / 4)
    bool     alertFlag = false;
    bool     antispoofFlag = false;
    uint64_t timestampMs = 0; // host timestamp when the subframe was completed

    // Time-anchoring info (populated by the NAV decoder for every valid
    // subframe). Together: at `preamble_pc_time` the GPS TOW was
    // `gps_tow_seconds_at_preamble`.
    double   gps_tow_seconds_at_preamble = 0.0;
    std::chrono::system_clock::time_point preamble_pc_time;
};

class NavDecoder {
public:
    // Called once per detected subframe. The callback is invoked on the
    // decoder thread; it must be cheap or post work to a queue.
    using SubframeCallback = std::function<void(const SubframeInfo&)>;

    explicit NavDecoder(int prn) : prn_(prn) {}

    // Feed one soft symbol (one per ms). Internally tries to find the 20 ms
    // bit boundary, accumulate full nav bits, then search for the preamble
    // and parse TLM + HOW.
    void feedSoftSymbol(int8_t s);

    // Feed a batch.
    void feed(const std::vector<int8_t>& syms) {
        for (auto s : syms) feedSoftSymbol(s);
    }

    void setSubframeCallback(SubframeCallback cb) {
        std::lock_guard<std::mutex> l(mu_);
        cb_ = std::move(cb);
    }

    // Statistics for the GUI
    int  getBitsDecoded()      { std::lock_guard<std::mutex> l(mu_); return bitsDecoded_; }
    int  getSubframesDecoded() { std::lock_guard<std::mutex> l(mu_); return subframesDecoded_; }
    bool isBitSynced()         { std::lock_guard<std::mutex> l(mu_); return bitSynced_; }
    int  getBitPhase()         { std::lock_guard<std::mutex> l(mu_); return bitPhase_; }

    // The most recent (GPS_TOW, PC_time) anchor extracted by this channel.
    // `valid` is false until the first subframe is decoded.
    TimeFix getLastTimeFix()   { std::lock_guard<std::mutex> l(mu_); return lastTimeFix_; }

private:
    void tryBitSync();
    void emitBit(int8_t bit);
    void searchPreamble();

    int prn_;
    std::mutex mu_;

    // Symbol-level buffer for bit synchronisation. We hold ~200 symbols
    // (= 10 nav bits) and look for the 20-symbol alignment that maximises
    // the integrated magnitude.
    std::deque<int8_t> symbolBuffer_;
    bool   bitSynced_  = false;
    int    bitPhase_   = 0;        // offset 0..19 into the 20 symbols / bit cycle
    int    symbolCount_ = 0;

    // Bit-level buffer
    std::deque<int8_t> bitBuffer_;        // ±1 bits with arbitrary polarity
    // Parallel deque holding the PC time at which each bit was emitted.
    // Used to anchor a GPS TOW to a PC clock instant when a preamble is
    // detected at a known bit index.
    std::deque<std::chrono::system_clock::time_point> bitTimes_;
    int  bitsDecoded_      = 0;
    int  subframesDecoded_ = 0;
    // Monotonic count of nav bits emitted since bit-sync was achieved. Used
    // to derive a stable absolute index that survives front-trims of the
    // bit buffer. lastPreambleAbsIdx_ is the value of bitsEmittedAbs_ - 60
    // at the moment a preamble was confirmed (i.e., the absolute index of
    // that preamble's first bit).
    int64_t bitsEmittedAbs_   = 0;
    int64_t lastPreambleAbsIdx_ = -1;
    bool polarityInverted_ = false;       // resolved by parity check

    TimeFix lastTimeFix_;

    SubframeCallback cb_;
};

} // namespace gps
