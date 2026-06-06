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

void NavDecoder::feedSoftSymbol(int8_t s) {
    std::lock_guard<std::mutex> l(mu_);

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
    int8_t adjusted = polarityInverted_ ? (int8_t)(-bit) : bit;
    bitBuffer_.push_back(adjusted);
    while (bitBuffer_.size() > 1600) bitBuffer_.pop_front();

    if (bitBuffer_.size() >= 60) {
        searchPreamble();
    }
}

void NavDecoder::searchPreamble() {
    int n = (int)bitBuffer_.size();
    int searchStart = std::max(0, n - 50);
    for (int i = searchStart; i + 60 <= n; i++) {
        bool match    = true;
        bool matchInv = true;
        for (int k = 0; k < 8; k++) {
            int8_t b = bitBuffer_[i + k];
            if (b !=  PREAMBLE[k]) match    = false;
            if (b != -PREAMBLE[k]) matchInv = false;
        }
        if (!match && !matchInv) continue;
        if (matchInv && !match) {
            // Inverted preamble found: flip polarity globally and the new
            // buffer will match the canonical preamble at this position.
            polarityInverted_ = !polarityInverted_;
            for (auto& b : bitBuffer_) b = (int8_t)(-b);
        }

        // Filter out chance matches: subframes are exactly 300 bits apart.
        if (lastPreambleIdx_ >= 0) {
            int delta = (i - lastPreambleIdx_);
            if (delta != 300) continue;
        }

        int howStart = i + 30;
        if (howStart + 30 > (int)bitBuffer_.size()) return;

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
            SubframeInfo info;
            info.prn           = prn_;
            info.subframeId    = (int)sfid;
            info.tow_count     = tow;
            info.alertFlag     = alert;
            info.antispoofFlag = antiSpoof;
            info.timestampMs   = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch()).count();

            subframesDecoded_++;
            lastPreambleIdx_ = i;

            if (cb_) cb_(info);
        }
        return;
    }
}

} // namespace gps
