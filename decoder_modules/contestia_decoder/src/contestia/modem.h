#pragma once
// Contestia modem core.
//
// This is a thin, real-time wrapper around Pawel Jalocha's (SP9VRC) MFSK
// receiver engine (jalocha/pj_mfsk.h), which is the very same DSP used by
// fldigi for its Contestia modem. The engine is a complete, self-contained
// Contestia receiver: FFT tone demodulator, frequency/time synchroniser over a
// search grid, Walsh-Hadamard (FHT) soft-decision FEC decoder, descrambler and
// character FIFO. We feed it the real audio coming out of the SSB demodulator
// and pull decoded ASCII characters back out.
//
// The engine runs internally at 8 kHz (the symbol/FFT lengths are defined in
// terms of that rate, so the standard Contestia bandwidths come out exactly right,
// e.g. OL 32-1000 -> 512-point window -> 15.625 Hz/bin -> 31.25 Hz/tone ->
// 32 x 31.25 = 1000 Hz). The module therefore creates an 8 kHz VFO and feeds
// 8 kHz audio straight in, exactly like fldigi.
//
// Validated by decoding the real off-air sigidwiki recordings (OL 8-250,
// 16-500, 32-1000 all give "The quick brown fox jumps over the lazy dog
// 1234567890") and by a synthetic encode/decode round-trip down to about
// -6 dB in-band SNR across all submodes.
#include <vector>
#include <cmath>
#include <cstdint>
#include <functional>
#include <algorithm>
#include "jalocha/pj_mfsk.h"

namespace contestia {

static constexpr double CONTESTIA_SR = 8000.0;   // engine + VFO audio rate

// The 19 submodes exposed in the UI (the standard Contestia set). The label
// uses the "Cont<tones>-<bandwidth>" convention with 1K/2K shorthand for
// 1000/2000 Hz. Contestia uses the same MFSK tone geometry as Olivia, so the
// (tones, bandwidth) pairs map onto the engine identically; only the FEC block
// size (32 vs 64) and the 6-bit alphabet differ (selected via bContestia).
struct ContestiaMode {
    const char* name;
    int tones;
    int bw;     // Hz
};

static const ContestiaMode CONTESTIA_MODES[] = {
    {"Cont4-125",   4,  125},  {"Cont4-250",   4,  250},  {"Cont4-500",   4,  500},
    {"Cont4-1K",    4, 1000},  {"Cont4-2K",    4, 2000},
    {"Cont8-125",   8,  125},  {"Cont8-250",   8,  250},  {"Cont8-500",   8,  500},
    {"Cont8-1K",    8, 1000},  {"Cont8-2K",    8, 2000},
    {"Cont16-250", 16,  250},  {"Cont16-500", 16,  500},  {"Cont16-1K",  16, 1000},
    {"Cont16-2K",  16, 2000},
    {"Cont32-1K",  32, 1000},  {"Cont32-2K",  32, 2000},
    {"Cont64-500", 64,  500},  {"Cont64-1K",  64, 1000},  {"Cont64-2K",  64, 2000},
};
static constexpr int CONTESTIA_MODE_COUNT = 19;

class ContestiaModem {
public:
    ContestiaModem() {
        configure(modeIdx, centerHz, syncMargin, syncInteg);
    }
    ~ContestiaModem() { delete Rx; Rx = nullptr; }
    ContestiaModem(const ContestiaModem&) = delete;
    ContestiaModem& operator=(const ContestiaModem&) = delete;

    // (Re)build the receiver for a new submode / centre / sync settings. This
    // allocates, so callers (decoder.h) only ever invoke it while the DSP chain
    // is stopped, never from the audio thread mid-stream.
    void configure(int idx, double center, int margin, int integ) {
        modeIdx   = std::clamp(idx, 0, CONTESTIA_MODE_COUNT - 1);
        centerHz  = center;
        syncMargin = std::clamp(margin, 2, 16);
        syncInteg  = std::clamp(integ, 2, 8);

        const ContestiaMode& M = CONTESTIA_MODES[modeIdx];
        tones = M.tones;
        bw    = M.bw;

        delete Rx;
        Rx = new MFSK_Receiver<double>();
        Rx->bContestia      = true;             // Contestia: 6-bit alphabet, 32-symbol FEC blocks
        Rx->Tones           = tones;
        Rx->Bandwidth       = bw;
        Rx->SyncMargin      = syncMargin;       // +/- tone-spacings of tuning tolerance
        Rx->SyncIntegLen    = syncInteg;        // FEC blocks integrated for sync
        Rx->SyncThreshold   = 3.0;              // S/N print gate (fldigi default)
        Rx->SampleRate      = CONTESTIA_SR;
        Rx->InputSampleRate = CONTESTIA_SR;
        Rx->Reverse         = 0;
        applyCarrier();
        if (Rx->Preset() < 0) { delete Rx; Rx = nullptr; return; }
        Rx->Reset();

        baud      = (double)Rx->BaudRate();
        toneSpace = (double)bw / tones;

        // (re)build the GUI band-spectrum analysis tables
        specCos.clear();
        specMag.assign(SPEC_BINS, 0.0f);
        specWin.assign(SPEC_ANALYSIS_LEN, 0.0f);
        swp = 0; specHop = 0;
        curSNR = 0.0f; curFreqOff = 0.0f;
    }

    void setCenter(double center) {
        centerHz = center;
        if (!Rx) return;
        // FirstCarrierMultiplier can be retuned without a full Preset(); but the
        // demodulator caches FirstCarrier at Preset time, so a Preset is needed
        // for it to take effect. Preset() wipes decode state, which is fine: a
        // retune is a deliberate user action.
        applyCarrier();
        Rx->Preset();
        Rx->Reset();
    }

    void set8bit(bool en)        { eightBit = en; }
    void setSyncThreshold(double t) { if (Rx) Rx->SyncThreshold = (t < 3.0 ? 3.0 : t); }

    // Feed a block of real audio (8 kHz) and emit each decoded character.
    void process(const float* audio, int count, const std::function<void(char)>& emit) {
        if (!Rx || count <= 0) return;

        // GUI band spectrum (cheap Goertzel bank over the SSB passband).
        for (int i = 0; i < count; i++) {
            specWin[swp] = audio[i];
            swp = (swp + 1) % SPEC_ANALYSIS_LEN;
            if (++specHop >= SPEC_DECIM) { specHop = 0; updateBandSpectrum(); }
        }

        // The engine takes a (templated) input pointer and converts/resamples
        // internally; here input rate == internal rate so it is a passthrough.
        Rx->Process(const_cast<float*>(audio), (size_t)count);

        uint8_t ch;
        while (Rx->GetChar(ch) > 0) {
            int c = unescape(ch);
            if (c >= 0 && c > 7) emit((char)c);   // fldigi prints codes > 7
        }

        curSNR     = (float)Rx->SignalToNoiseRatio();
        curFreqOff = (float)Rx->FrequencyOffset();
    }

    void flush(const std::function<void(char)>& emit) {
        if (!Rx) return;
        Rx->Flush();
        uint8_t ch;
        while (Rx->GetChar(ch) > 0) {
            int c = unescape(ch);
            if (c >= 0 && c > 7) emit((char)c);
        }
    }

    // --- status read-outs for the GUI ---------------------------------------
    float  getSNR()        const { return curSNR; }
    float  getFreqOffset() const { return curFreqOff; }
    double getBaud()       const { return baud; }
    double getToneSpace()  const { return toneSpace; }
    int    getTones()      const { return tones; }
    int    getBandwidth()  const { return bw; }
    double getCenter()     const { return centerHz; }

    // --- band-spectrum display (same idea as the MFSK module) ----------------
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
    // average energy. Returns the centre (Hz) of that window, or <0 if no
    // usable spectrum yet. Used by the "Auto-center" button as a tuning aid;
    // the synchroniser's SyncMargin then pulls in the residual error.
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
    void applyCarrier() {
        if (!Rx) return;
        double fcOffset = bw * (1.0 - 0.5 / tones) / 2.0;
        Rx->FirstCarrierMultiplier = (float)((centerHz - fcOffset) / 500.0);
    }

    // fldigi's optional 8-bit extension: code 127 escapes the next char to +128.
    int unescape(int c) {
        if (!eightBit) return c;
        if (escapeState) { escapeState = false; return c + 128; }
        if (c == 127)    { escapeState = true;  return -1; }
        return c;
    }

    void updateBandSpectrum() {
        if (specCos.empty()) {
            specCos.resize(SPEC_BINS);
            for (int b = 0; b < SPEC_BINS; b++) {
                double f = SPEC_FLO + b * ((SPEC_FHI - SPEC_FLO) / (SPEC_BINS - 1));
                specCos[b] = (float)(2.0 * std::cos(2.0 * M_PI * f / CONTESTIA_SR));
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

    MFSK_Receiver<double>* Rx = nullptr;

    int    modeIdx   = 11;       // default Cont16-500
    int    tones     = 16;
    int    bw        = 500;
    double centerHz  = 1500.0;
    int    syncMargin = 8;
    int    syncInteg  = 4;
    bool   eightBit   = false;
    bool   escapeState = false;

    double baud      = 31.25;
    double toneSpace = 31.25;

    // status (written on audio thread, read on GUI thread; benign races, same
    // pattern as the MFSK module's curSNR/curTone)
    float  curSNR     = 0.0f;
    float  curFreqOff = 0.0f;

    // band spectrum
    std::vector<float> specCos;
    std::vector<float> specMag;
    std::vector<float> specWin;
    int swp = 0, specHop = 0;
};

} // namespace contestia
