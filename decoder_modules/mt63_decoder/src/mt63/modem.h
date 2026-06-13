#pragma once
// MT63 modem core.
//
// A thin, real-time wrapper around Pawel Jalocha's (SP9VRC) MT63 receiver
// engine (jalocha/mt63base.{h,cpp} + jalocha/dsp.{h,cpp}), which is the very
// same DSP that fldigi uses for its MT63 modem. The engine is a complete,
// self-contained MT63 receiver: I/Q split + anti-alias decimation, FFT tone
// demodulator, time/frequency synchroniser over a carrier search grid,
// differential BPSK detection on 64 carriers, time+frequency de-interleaver and
// a Walsh-Hadamard (FHT) soft-decision FEC decoder feeding a character output.
// We feed it the real audio coming out of the SSB demodulator and pull decoded
// ASCII characters back out.
//
// The engine runs internally at 8 kHz (its FFT/symbol lengths are defined in
// terms of that rate, so the standard MT63 bandwidths come out exactly right:
// 64 carriers over 500/1000/2000 Hz). The module therefore creates an 8 kHz VFO
// and feeds 8 kHz audio straight in, exactly like fldigi.
//
// MT63 has six submodes: three bandwidths (500/1000/2000 Hz) x two interleave
// depths (Short = 32 symbols, Long = 64 symbols). Wider bandwidth = faster
// (more symbols/s); longer interleave = more robust to fading/impulse noise at
// the cost of a longer acquisition + fill delay before text appears.
//
// Validated by a synthetic encode/decode round-trip (the bundled
// mt63_roundtrip tool) across all six submodes down to ~0 dB in-band SNR, and
// by decoding off-air / generated WAV recordings with mt63_decode_wav.
#include <vector>
#include <cmath>
#include <cstdint>
#include <functional>
#include <algorithm>
#include "jalocha/dsp.h"
#include "jalocha/mt63base.h"

namespace mt63 {

static constexpr double MT63_SR = 8000.0;   // engine + VFO audio rate

// The six submodes exposed in the UI, matching fldigi's MT63 mode list.
//   name        = label shown in the combo
//   bw          = occupied bandwidth (Hz): 500 / 1000 / 2000
//   longIntlv   = 0 (Short, 32-symbol interleave) / 1 (Long, 64-symbol)
struct Mt63Mode {
    const char* name;
    int bw;
    int longIntlv;
};

static const Mt63Mode MT63_MODES[] = {
    {"MT63-500S",   500, 0},
    {"MT63-500L",   500, 1},
    {"MT63-1000S", 1000, 0},
    {"MT63-1000L", 1000, 1},
    {"MT63-2000S", 2000, 0},
    {"MT63-2000L", 2000, 1},
};
static constexpr int MT63_MODE_COUNT = 6;

class Mt63Modem {
public:
    Mt63Modem() {
        configure(modeIdx, centerHz, integ);
    }
    ~Mt63Modem() { delete Rx; Rx = nullptr; }
    Mt63Modem(const Mt63Modem&) = delete;
    Mt63Modem& operator=(const Mt63Modem&) = delete;

    // (Re)build the receiver for a new submode / centre / integration. This
    // allocates, so callers (decoder.h) only ever invoke it while the DSP chain
    // is stopped, never from the audio thread mid-stream.
    void configure(int idx, double center, int integration) {
        modeIdx  = std::clamp(idx, 0, MT63_MODE_COUNT - 1);
        centerHz = center;
        integ    = std::clamp(integration, 8, 64);

        const Mt63Mode& M = MT63_MODES[modeIdx];
        bw        = M.bw;
        longIntlv = M.longIntlv;

        delete Rx;
        Rx = new MT63rx();
        if (Rx->Preset(centerHz, bw, longIntlv, integ, nullptr) < 0) {
            delete Rx; Rx = nullptr; return;
        }

        baud      = bw / 100.0;          // symbols/s (10 for 1000 Hz)
        toneSpace = bw / 64.0;           // carrier spacing (Hz)

        // (re)build the GUI band-spectrum analysis tables
        specCos.clear();
        specMag.assign(SPEC_BINS, 0.0f);
        specWin.assign(SPEC_ANALYSIS_LEN, 0.0f);
        swp = 0; specHop = 0;
        curSNR = 0.0f; curFreqOff = 0.0f; curLock = false; curConf = 0.0f;
    }

    void setCenter(double center) {
        centerHz = center;
        if (!Rx) return;
        // The demodulator caches FirstDataCarr at Preset time, so a retune needs
        // a fresh Preset. Preset() wipes decode state, which is fine: a retune is
        // a deliberate user action.
        Rx->Preset(centerHz, bw, longIntlv, integ, nullptr);
    }

    void set8bit(bool en) { eightBit = en; }

    // Feed a block of real audio (8 kHz) and emit each decoded character.
    void process(const float* audio, int count, const std::function<void(char)>& emit) {
        if (!Rx || count <= 0) return;

        // GUI band spectrum (cheap Goertzel bank over the SSB passband).
        for (int i = 0; i < count; i++) {
            specWin[swp] = audio[i];
            swp = (swp + 1) % SPEC_ANALYSIS_LEN;
            if (++specHop >= SPEC_DECIM) { specHop = 0; updateBandSpectrum(); }
        }

        // Hand the audio to the engine as a double buffer.
        inBuf.EnsureSpace(count);
        inBuf.Len = count;
        for (int i = 0; i < count; i++) inBuf.Data[i] = (double)audio[i];
        Rx->Process(&inBuf);

        for (int i = 0; i < Rx->Output.Len; i++) {
            int c = unescape((unsigned char)Rx->Output.Data[i]);
            if (c >= 0) emit((char)c);
        }

        curSNR     = (float)Rx->FEC_SNR();
        curFreqOff = (float)Rx->TotalFreqOffset();
        curLock    = Rx->SYNC_LockStatus() != 0;
        curConf    = (float)Rx->SYNC_Confidence();
    }

    // --- status read-outs for the GUI ---------------------------------------
    float  getSNR()        const { return curSNR; }
    float  getFreqOffset() const { return curFreqOff; }
    bool   getLock()       const { return curLock; }
    float  getConfidence() const { return curConf; }
    double getBaud()       const { return baud; }
    double getToneSpace()  const { return toneSpace; }
    int    getBandwidth()  const { return bw; }
    double getCenter()     const { return centerHz; }

    // --- band-spectrum display (same idea as the olivia/mfsk modules) --------
    static constexpr int    SPEC_BINS = 160;
    static constexpr double SPEC_FLO  = 300.0;
    static constexpr double SPEC_FHI  = 2700.0;
    static constexpr int    SPEC_ANALYSIS_LEN = 1024;  // ~7.8 Hz resolution @ 8 kHz
    static constexpr int    SPEC_DECIM = 64;           // recompute every 64 samples

    int getBandSpectrum(float* out, int n) const {
        int c = std::min(n, (int)specMag.size());
        for (int i = 0; i < c; i++) out[i] = specMag.empty() ? 0.0f : specMag[i];
        return c;
    }
    double getBandFlo()  const { return SPEC_FLO; }
    double getBandFhi()  const { return SPEC_FHI; }
    double getEdgeFreq() const { return centerHz - bw / 2.0; }     // lower edge of signal
    double getToneSpan() const { return (double)bw; }              // full occupied width

    // One-shot centre estimate: slide a Bandwidth-wide window across the
    // current smoothed band spectrum and pick the window with the highest
    // average energy. Returns the centre (Hz) of that window, or <0 if no usable
    // spectrum yet. Used by the "Auto-center" button as a tuning aid; the
    // synchroniser's scan margin then pulls in the residual error.
    double estimateCenter() const {
        if (specMag.empty()) return -1.0;
        double binHz = (SPEC_FHI - SPEC_FLO) / (SPEC_BINS - 1);
        int winBins = std::max(1, (int)std::lround(bw / binHz));
        double total = 0; for (float v : specMag) total += v;
        if (total < 1e-9) return -1.0;
        double bestSum = -1; int bestStart = 0;
        double run = 0;
        for (int i = 0; i < SPEC_BINS; i++) {
            run += specMag[i];
            if (i >= winBins) run -= specMag[i - winBins];
            if (i >= winBins - 1) {
                if (run > bestSum) { bestSum = run; bestStart = i - winBins + 1; }
            }
        }
        double centerBin = bestStart + (winBins - 1) / 2.0;
        return SPEC_FLO + centerBin * binHz;
    }

private:
    // fldigi's optional 8-bit extension: code 127 escapes the next char to +128,
    // and in 8-bit mode control codes < 8 are suppressed. In 7-bit (default)
    // mode everything is passed through.
    int unescape(int c) {
        if (!eightBit) return c;
        if (escapeState) { escapeState = false; return c + 128; }
        if (c == 127)    { escapeState = true;  return -1; }
        if (c < 8)       { return -1; }
        return c;
    }

    void updateBandSpectrum() {
        if (specCos.empty()) {
            specCos.resize(SPEC_BINS);
            for (int b = 0; b < SPEC_BINS; b++) {
                double f = SPEC_FLO + b * ((SPEC_FHI - SPEC_FLO) / (SPEC_BINS - 1));
                specCos[b] = (float)(2.0 * std::cos(2.0 * M_PI * f / MT63_SR));
            }
        }
        int SL = SPEC_ANALYSIS_LEN;
        int start = swp;   // oldest sample (ring is exactly SL long)
        for (int b = 0; b < SPEC_BINS; b++) {
            double cw = specCos[b], s1 = 0, s2 = 0; int idx = start;
            for (int m = 0; m < SL; m++) {
                double s0 = specWin[idx] + cw * s1 - s2; s2 = s1; s1 = s0;
                if (++idx >= SL) idx = 0;
            }
            double p = s1 * s1 + s2 * s2 - cw * s1 * s2;
            float mag = (float)(p > 0 ? std::sqrt(p) : 0.0);
            specMag[b] = 0.7f * specMag[b] + 0.3f * mag;   // smooth
        }
    }

    MT63rx*    Rx = nullptr;
    double_buff inBuf;

    int    modeIdx   = 2;        // default MT63-1000S
    int    bw        = 1000;
    int    longIntlv = 0;
    double centerHz  = 1500.0;
    int    integ     = 32;
    bool   eightBit   = false;
    bool   escapeState = false;

    double baud      = 10.0;
    double toneSpace = 15.625;

    // status (written on audio thread, read on GUI thread; benign races, same
    // pattern as the olivia/mfsk modules)
    float  curSNR     = 0.0f;
    float  curFreqOff = 0.0f;
    bool   curLock    = false;
    float  curConf    = 0.0f;

    // band spectrum
    std::vector<float> specCos;
    std::vector<float> specMag;
    std::vector<float> specWin;
    int swp = 0, specHop = 0;
};

} // namespace mt63
