// gps_nav.cpp -- 50 bps Navigation Message decoder

#include "gps_nav.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace gps {

// Subframe preamble: 8 bits, 0x8B = 10001011 ; in ±1 form
static const int8_t PREAMBLE[8] = { +1, -1, -1, -1, +1, -1, +1, +1 };

// Window of symbols we keep while searching for bit boundaries
static constexpr int SYMBOL_WINDOW = 200;

void NavDecoder::feedSoftSymbol(int8_t s, int msCount, double codePhase) {
    std::lock_guard<std::mutex> l(mu_);

    // Remember the latest symbol-level tracker state. emitBit() will use
    // this as the per-bit state (= state of the 20th symbol = bit boundary).
    lastSymMsCount_   = msCount;
    lastSymCodePhase_ = codePhase;

    if (!bitSynced_) {
        symbolBuffer_.push_back(s);
        if ((int)symbolBuffer_.size() > SYMBOL_WINDOW) symbolBuffer_.pop_front();
        if ((int)symbolBuffer_.size() >= SYMBOL_WINDOW) {
            tryBitSync();
        }
        return;
    }

    // We are bit-synced; integrate 20 symbols per nav bit
    symbolBuffer_.push_back(s);
    if ((int)symbolBuffer_.size() >= 20) {
        int sum = 0;
        for (int i = 0; i < 20; i++) sum += symbolBuffer_[i];
        for (int i = 0; i < 20; i++) symbolBuffer_.pop_front();
        int8_t bit = (sum >= 0) ? +1 : -1;
        emitBit(bit);
    }
}

void NavDecoder::tryBitSync() {
    int bestPhase = 0;
    long bestMetric = -1;
    for (int phase = 0; phase < 20; phase++) {
        long metric = 0;
        int b = 0;
        while (b + phase + 19 < (int)symbolBuffer_.size()) {
            long sum = 0;
            for (int j = 0; j < 20; j++) sum += symbolBuffer_[b + phase + j];
            metric += std::abs(sum);
            b += 20;
        }
        if (metric > bestMetric) {
            bestMetric = metric;
            bestPhase  = phase;
        }
    }

    for (int i = 0; i < bestPhase; i++) symbolBuffer_.pop_front();
    bitPhase_  = bestPhase;
    bitSynced_ = true;
}

void NavDecoder::emitBit(int8_t bit) {
    bitsDecoded_++;
    bitsEmittedAbs_++;
    // The bit's nominal mid-point in PC time. The 20 symbols that went into
    // this bit cover the past 20 ms in the IQ stream, so the bit centre is
    // 10 ms ago. Any constant DSP-pipeline latency on top of that biases
    // every fix identically, which is fine for time-of-day display; only
    // bias variation degrades accuracy.
    auto bitTime = std::chrono::system_clock::now() - std::chrono::milliseconds(10);

    int8_t adjusted = polarityInverted_ ? (int8_t)(-bit) : bit;
    bitBuffer_.push_back(adjusted);
    bitTimes_.push_back(bitTime);
    // Capture the tracker's chip-level state from the 20th symbol of this
    // bit (= the most recently fed symbol). This gives the chip count
    // corresponding to "end of this bit" in tracker time.
    bitMsCounts_.push_back(lastSymMsCount_);
    bitCodePhases_.push_back(lastSymCodePhase_);
    // bitBuffer_ / bitTimes_ / bitMsCounts_ / bitCodePhases_ kept in lockstep
    while ((int)bitBuffer_.size() > 1600) {
        bitBuffer_.pop_front();
        bitTimes_.pop_front();
        bitMsCounts_.pop_front();
        bitCodePhases_.pop_front();
    }

    if (bitBuffer_.size() >= 60) {
        searchPreamble();
    }
}

void NavDecoder::searchPreamble() {
    int n = (int)bitBuffer_.size();
    if (n < 60) return;

    // A preamble at bit-buffer position i is confirmable once we have the
    // full TLM (22 bits after preamble end) and HOW (30 bits more), i.e.
    // 60 bits total. Right after a new bit is emitted, the only position
    // where a preamble would NOW become confirmable is exactly i = n - 60
    // (the bit emitted 60 bits ago, whose subframe just finished filling
    // its first two 30-bit words). Checking that single position on every
    // emitted bit is sufficient and avoids buffer-trim index issues.
    int i = n - 60;

    bool match    = true;
    bool matchInv = true;
    for (int k = 0; k < 8; k++) {
        int8_t b = bitBuffer_[i + k];
        if (b !=  PREAMBLE[k]) match    = false;
        if (b != -PREAMBLE[k]) matchInv = false;
    }
    if (!match && !matchInv) return;
    if (matchInv && !match) {
        // Inverted preamble: flip polarity globally so subsequent decoding
        // works with the canonical sign convention.
        polarityInverted_ = !polarityInverted_;
        for (auto& b : bitBuffer_) b = (int8_t)(-b);
    }

    // Absolute bit index of this preamble's first bit. bitsEmittedAbs_ was
    // already incremented for the most recent bit, so the bit at relative
    // index i = n-60 has absolute index (bitsEmittedAbs_ - 60).
    int64_t absIdx = bitsEmittedAbs_ - 60;

    // Subframes are exactly 300 bits apart. Reject chance matches by
    // requiring delta to be a positive multiple of 300 (handles missed
    // intermediate subframes gracefully, e.g. after a brief tracking
    // glitch).
    if (lastPreambleAbsIdx_ >= 0) {
        int64_t delta = absIdx - lastPreambleAbsIdx_;
        if (delta <= 0 || (delta % 300) != 0) return;
    }

    int howStart = i + 30;
    // Guaranteed: howStart + 30 = i + 60 = n, in range.

    auto bitsToU = [&](int offset, int nbits) -> uint32_t {
        uint32_t v = 0;
        for (int k = 0; k < nbits; k++) {
            v <<= 1;
            if (bitBuffer_[offset + k] > 0) v |= 1;
        }
        return v;
    };

    uint32_t tow      = bitsToU(howStart + 0, 17);
    bool     alert    = (bitBuffer_[howStart + 17] > 0);
    bool     antiSpoof= (bitBuffer_[howStart + 18] > 0);
    uint32_t sfid     = bitsToU(howStart + 19, 3);

    if (sfid >= 1 && sfid <= 5) {
        // The HOW gives the TOW count at the start of the NEXT subframe
        // (a 17-bit truncated Z-count, so TOW_seconds(next) = tow * 6).
        // The preamble we just located is at the start of THIS subframe,
        // so TOW(this) = TOW(next) - 6 s. Handle end-of-week wrap.
        constexpr uint32_t SUBFRAMES_PER_WEEK = 100800; // 604800 / 6
        uint32_t towCountThis =
            (tow + SUBFRAMES_PER_WEEK - 1) % SUBFRAMES_PER_WEEK;
        double gpsTowSeconds = (double)towCountThis * 6.0;

        // PC time at preamble bit 0. bitTimes_ runs parallel to bitBuffer_,
        // so the same relative index applies.
        std::chrono::system_clock::time_point preambleTime;
        int    preambleMsCount   = 0;
        double preambleCodePhase = 0.0;
        if (i < (int)bitTimes_.size()) {
            preambleTime      = bitTimes_[i];
            preambleMsCount   = bitMsCounts_[i];
            preambleCodePhase = bitCodePhases_[i];
        } else {
            preambleTime = std::chrono::system_clock::now();
        }

        SubframeInfo info;
        info.prn           = prn_;
        info.subframeId    = (int)sfid;
        info.tow_count     = tow;
        info.alertFlag     = alert;
        info.antispoofFlag = antiSpoof;
        info.timestampMs   = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch()).count();
        info.gps_tow_seconds_at_preamble = gpsTowSeconds;
        info.preamble_pc_time            = preambleTime;

        subframesDecoded_++;
        lastPreambleAbsIdx_ = absIdx;

        // Update the time-fix slot used by the GUI / system-clock setter
        lastTimeFix_.valid           = true;
        lastTimeFix_.prn             = prn_;
        lastTimeFix_.gps_tow_seconds = gpsTowSeconds;
        lastTimeFix_.pc_time         = preambleTime;

        // Update the tracker-state anchor used by the PVT engine. This
        // pairs a GPS TOW with the tracker's chip-level state at the
        // SAME moment, so later snapshots of (msCount, codePhase) can be
        // converted to satellite transmit time precisely.
        trackerAnchor_.valid            = true;
        trackerAnchor_.gps_tow_seconds  = gpsTowSeconds;
        trackerAnchor_.msCount          = preambleMsCount;
        trackerAnchor_.codePhase        = preambleCodePhase;
        trackerAnchor_.pc_time          = preambleTime;

        // Queue this subframe for full parity-checked decoding once all
        // 300 bits have arrived.
        pendingFullSubframes_.push_back(absIdx);

        if (cb_) cb_(info);
    }

    // ----- Process pending full-subframe candidates --------------------
    // A candidate's preamble is at absolute index `candAbs`. We need 300
    // bits from candAbs onwards, i.e., bitsEmittedAbs_ >= candAbs + 300.
    while (!pendingFullSubframes_.empty()) {
        int64_t candAbs = pendingFullSubframes_.front();
        if (bitsEmittedAbs_ - candAbs < 300) break;
        // Map absolute candidate index to current buffer position.
        int64_t bufStartAbs = bitsEmittedAbs_ - (int64_t)bitBuffer_.size();
        int64_t relIdx = candAbs - bufStartAbs;
        pendingFullSubframes_.pop_front();
        if (relIdx < 0 || relIdx + 300 > (int64_t)bitBuffer_.size()) continue;
        // Copy out the 300 bits and run full parity-checked decode.
        int8_t bits300[300];
        for (int k = 0; k < 300; k++) bits300[k] = bitBuffer_[(size_t)(relIdx + k)];
        SubframeData sf;
        if (decodeSubframeWords(bits300, prn_, false, false, sf)) {
            fullSubframesDecoded_++;
            applySubframeToEphemeris(sf, ephemeris_);
            ephemeris_.prn           = prn_;
            ephemeris_.received_time = std::chrono::system_clock::now();
        }
    }
}

} // namespace gps
