#pragma once
#include "../decoder.h"
#include "../common/tone_mixer.h"
#include "../common/varicode.h"
#include "../common/mfsk_varicode.h"
#include "../common/viterbi_k7.h"
#include "../common/auto_tune.h"

#include <string>
#include <mutex>
#include <cstring>
#include <algorithm>

#include <imgui.h>
#include <gui/style.h>
#include <gui/widgets/waterfall.h>
#include <gui/widgets/symbol_diagram.h>
#include <utils/flog.h>

#include <signal_path/vfo_manager.h>
#include <dsp/stream.h>
#include <dsp/types.h>
#include <dsp/demod/ssb.h>
#include <dsp/demod/quadrature.h>
#include <dsp/demod/psk.h>
#include <dsp/multirate/rational_resampler.h>
#include <dsp/noise_reduction/power_squelch.h>
#include <dsp/sink/handler_sink.h>

// ---- Audio chain common parameters --------------------------------------
// Audio sample rate is 24 kHz so the NFM mode (12.5 kHz channel) fits below
// Nyquist. SSB modes only use the lower 3 kHz of this band but the higher SR
// costs little here and keeps the DSP chain a single rate.
#define PSK_AUDIO_SR      24000.0
#define PSK_SSB_BW        2800.0     // SSB audio passband (3 kHz nominal)
#define PSK_NFM_BW        12500.0    // standard narrow-FM channel width
#define PSK_NFM_DEV       5000.0     // FM peak deviation (Carson ~13 kHz)
#define PSK_DIAG_LEN      512        // scope length
#define PSK_SQUELCH_MIN   -100.0f    // dB: effectively "always open"
#define PSK_SQUELCH_MAX   0.0f       // dB

namespace fldigi {

    // Mode-specific PSK profile
    struct PSKProfile {
        const char* name;
        double      baud;        // symbol rate
        double      basebandSR;  // baseband sample rate fed to the PSK demod
        double      rrcBeta;     // RRC roll-off
        bool        fec;         // true for PSK63F (Viterbi K=7 R=1/2)
    };

    // sps = basebandSR / baud is kept >= 8 for all modes.
    static inline const PSKProfile& pskProfile(int idx) {
        static const PSKProfile profiles[] = {
            { "PSK31",   31.25,  2000.0, 0.5, false }, // 64 sps
            { "PSK63",   62.5,   2000.0, 0.5, false }, // 32 sps
            { "PSK63F",  62.5,   2000.0, 0.5, true  }, // 32 sps + Viterbi
            { "PSK125",  125.0,  2000.0, 0.5, false }, // 16 sps
            { "PSK250",  250.0,  4000.0, 0.5, false }, // 16 sps
            { "PSK500",  500.0,  8000.0, 0.5, false }, // 16 sps
            { "PSK1000", 1000.0, 8000.0, 0.5, false }, //  8 sps
        };
        return profiles[idx];
    };
    static constexpr int PSK_PROFILE_COUNT = 7;

    // VFO geometry per sideband / mode. Standard convention:
    //  - USB: dial freq sits at the LOWER edge, audio passband extends upward
    //  - LSB: dial freq sits at the UPPER edge, audio passband extends downward
    //  - NFM: dial freq is centered in a wider channel
    struct VfoGeometry {
        int    reference;
        double bandwidth;
    };

    static inline VfoGeometry vfoGeometryFor(VfoMode mode) {
        switch (mode) {
        case VFO_MODE_USB: return { ImGui::WaterfallVFO::REF_LOWER,  PSK_SSB_BW };
        case VFO_MODE_LSB: return { ImGui::WaterfallVFO::REF_UPPER,  PSK_SSB_BW };
        case VFO_MODE_NFM: return { ImGui::WaterfallVFO::REF_CENTER, PSK_NFM_BW };
        }
        return { ImGui::WaterfallVFO::REF_LOWER, PSK_SSB_BW };
    }

}

class BPSKDecoder : public Decoder {
public:
    BPSKDecoder(const std::string& name, VFOManager::VFO* vfo, int profileIdx)
        : diag(1.0f, PSK_DIAG_LEN), profile(fldigi::pskProfile(profileIdx))
    {
        this->name = name;
        this->vfo  = vfo;

        applyVfoGeometry(vfoMode);
        buildDSP();
    }

    ~BPSKDecoder() { stop(); }

    void showMenu() override {
        float menuWidth = ImGui::GetContentRegionAvail().x;

        // Recovered-symbol scope: BPSK shows two clusters at +/-1 on the real axis
        ImGui::FillWidth();
        diag.draw();

        ImGui::TextUnformatted("Decoded text:");
        ImGui::BeginChild(("fldigi_psk_text_" + name).c_str(), ImVec2(menuWidth, 150.0f), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        {
            std::lock_guard<std::mutex> lck(textMtx);
            ImGui::TextWrapped("%s", text.c_str());
            if (scrollDown) {
                ImGui::SetScrollHereY(1.0f);
                scrollDown = false;
            }
        }
        ImGui::EndChild();

        if (ImGui::Button(("Clear##fldigi_psk_clear_" + name).c_str(), ImVec2(menuWidth, 0))) {
            std::lock_guard<std::mutex> lck(textMtx);
            text.clear();
        }
    }

    void setVFO(VFOManager::VFO* vfo) override {
        bool wasRunning = running;
        if (wasRunning) { stop(); }
        this->vfo = vfo;
        applyVfoGeometry(vfoMode);
        squelch.setInput(vfo->output);
        if (wasRunning) { start(); }
    }

    void setVfoMode(VfoMode mode) override {
        if (mode == vfoMode) { return; }
        bool wasRunning = running;
        if (wasRunning) { stop(); }
        vfoMode = mode;
        applyVfoGeometry(vfoMode);
        if (wasRunning) { start(); }
    }

    void setAFFreq(double freq) override {
        afFreq = freq;
        if (running) { mixer.setFreq(afFreq); }
    }

    void start() override {
        if (running) { return; }

        squelch.setInput(vfo->output);
        squelch.setLevel(currentSquelchLevel());
        squelch.start();

        if (vfoMode == VFO_MODE_NFM) {
            nfm.setInput(&squelch.out);
            nfm.start();
            autoTune.setInput(&nfm.out);
            nfmActive = true;
        }
        else {
            ssb.setMode((vfoMode == VFO_MODE_LSB) ? dsp::demod::SSB<float>::LSB
                                                  : dsp::demod::SSB<float>::USB);
            ssb.setInput(&squelch.out);
            ssb.start();
            autoTune.setInput(&ssb.out);
            nfmActive = false;
        }

        autoTune.start();
        mixer.setInput(&autoTune.out);
        mixer.setFreq(afFreq);
        mixer.start();
        if (needResamp) { resamp.start(); }
        psk.start();
        symSink.start();

        running = true;
    }

    void stop() override {
        if (!running) { return; }
        symSink.stop();
        psk.stop();
        if (needResamp) { resamp.stop(); }
        mixer.stop();
        autoTune.stop();
        if (nfmActive) { nfm.stop(); }
        else           { ssb.stop(); }
        squelch.stop();
        running = false;
    }

    // Public squelch controls
    void setSquelchEnabled(bool en) override {
        squelchEnabled = en;
        if (running) { squelch.setLevel(currentSquelchLevel()); }
    }

    void setSquelchLevel(float dB) override {
        squelchLevel = std::clamp(dB, PSK_SQUELCH_MIN, PSK_SQUELCH_MAX);
        if (running) { squelch.setLevel(currentSquelchLevel()); }
    }

    bool  getSquelchEnabled() const override { return squelchEnabled; }
    float getSquelchLevel()   const override { return squelchLevel; }

    // Auto-tune controls
    void  beginAutoTune() override { if (running) autoTune.beginScan(); }
    void  cancelAutoTune() override { autoTune.cancelScan(); }
    bool  isAutoTuning() const override { return autoTune.isScanning(); }
    float autoTuneProgress() const override { return autoTune.progress(); }
    bool  autoTuneReady() const override { return autoTune.isReady(); }
    float autoTuneResult() override {
        float af = autoTune.fetchResult();
        if (af > 0.0f) { setAFFreq(af); }
        return af;
    }

    // Band view passthrough
    int    getBandSpectrum(float* out, int n) const override { return autoTune.getBandSpectrum(out, n); }
    double getBandFlo() const override { return autoTune.getBandFlo(); }
    double getBandFhi() const override { return autoTune.getBandFhi(); }
    double getSignalBandwidth() const override { return profile.baud; }
    float  getAFFreq() const override { return (float)afFreq; }

private:
    // Reconfigure the waterfall VFO (reference + bandwidth + sample rate) for
    // the current VFO mode. Safe to call while DSP is stopped.
    void applyVfoGeometry(VfoMode mode) {
        auto g = fldigi::vfoGeometryFor(mode);

        // Widen limits temporarily, set new bandwidth + sample rate, then lock.
        vfo->setBandwidthLimits(100.0, 50000.0, false);
        vfo->setReference(g.reference);
        vfo->setSampleRate(PSK_AUDIO_SR, g.bandwidth);
        vfo->setBandwidth(g.bandwidth);
        vfo->setBandwidthLimits(g.bandwidth, g.bandwidth, true);
    }

    void buildDSP() {
        // Power squelch sits between the VFO and the demodulators (works on
        // the complex IQ stream, gating it to zero when avg power < threshold).
        // We leave it always in the chain and just lower the level when the
        // user disables squelch.
        squelch.init(vfo->output, currentSquelchLevel());

        // Mode demodulators (only one is started at a time). Both consume
        // from the squelch output.
        ssb.init(&squelch.out, dsp::demod::SSB<float>::USB, PSK_SSB_BW, PSK_AUDIO_SR,
                 100.0 / PSK_AUDIO_SR, 5.0 / PSK_AUDIO_SR);
        nfm.init(&squelch.out, PSK_NFM_DEV, PSK_AUDIO_SR);

        // Transparent auto-tune pass-through block. Sits on the real-valued
        // audio stream right before the mixer; it doesn't alter the audio
        // but accumulates a Goertzel spectrum on demand to find the PSK
        // signal's centre frequency.
        autoTune.init(&ssb.out, PSK_AUDIO_SR);

        // Audio tone -> complex baseband (mixer always runs at audio SR)
        mixer.init(&autoTune.out, afFreq, PSK_AUDIO_SR);

        // Decimate to mode-specific baseband rate
        needResamp = (profile.basebandSR < PSK_AUDIO_SR - 1.0);
        dsp::stream<dsp::complex_t>* pskIn = nullptr;
        if (needResamp) {
            resamp.init(&mixer.out, PSK_AUDIO_SR, profile.basebandSR);
            pskIn = &resamp.out;
        } else {
            pskIn = &mixer.out;
        }

        // BPSK demod: RRC + AGC + Costas (ORDER=2) + M&M clock recovery
        int sps = (int)(profile.basebandSR / profile.baud);
        int rrcTaps = 2 * sps + 1;

        double costasBW = 0.005;
        double omegaGain = 1e-6;
        double muGain    = 0.01;
        if (profile.baud >= 250.0) {
            costasBW  = 0.01;
            omegaGain = 1e-5;
            muGain    = 0.02;
        }

        psk.init(pskIn, profile.baud, profile.basebandSR,
                 rrcTaps, profile.rrcBeta,
                 1e-3,
                 costasBW,
                 omegaGain,
                 muGain,
                 0.01);

        symSink.init(&psk.out, symbolHandler, this);

        // Reset decoder state on (re)start so mode switches don't inherit
        // stale Viterbi survivors, Varicode shift registers, or last-symbol
        // phase from a previous run.
        prev = { 1.0f, 0.0f };
        viterbi.reset();
        psk_varicode.reset();
        mfsk_varicode.reset();
    }

    static void symbolHandler(dsp::complex_t* data, int count, void* ctx) {
        BPSKDecoder* _this = (BPSKDecoder*)ctx;

        // Scope: real axis
        {
            float* buf = _this->diag.acquireBuffer();
            for (int i = 0; i < count; i++) {
                buf[_this->diagIdx] = data[i].re;
                _this->diagIdx = (_this->diagIdx + 1) % PSK_DIAG_LEN;
            }
            _this->diag.releaseBuffer();
        }

        // Differential BPSK detection. BPSK constellation has only two
        // points (+1, 0) and (-1, 0); the phase-direction issue affecting
        // QPSK/8PSK does not apply here because 0 and pi are fixed points
        // under phase negation, so the dot product is unambiguous.
        for (int i = 0; i < count; i++) {
            dsp::complex_t s = data[i];
            float dot = (s.re * _this->prev.re) + (s.im * _this->prev.im);
            int rawBit = (dot > 0.0f) ? 1 : 0;
            _this->prev = s;

            if (_this->profile.fec) {
                // BPSK63F: each received raw bit is one of (c0, c1) from
                // the K=7 R=1/2 convolutional code. The Viterbi accumulates
                // pairs and emits info bits. Decoded info bits go to the
                // MFSK Varicode decoder (FLDIGI psk.cxx line 1118 routes
                // _pskr modes through varidec() instead of psk_varicode).
                int v = _this->viterbi.process(rawBit);
                if (v < 0) { continue; }
                int ch = _this->mfsk_varicode.process(v);
                if (ch < 0) { continue; }
                _this->appendChar(ch);
            } else {
                // Plain BPSK: rawBit goes straight to PSK Varicode.
                int ch = _this->psk_varicode.process(rawBit);
                if (ch < 0) { continue; }
                _this->appendChar(ch);
            }
        }
    }

    void appendChar(int ch) {
        std::lock_guard<std::mutex> lck(textMtx);
        if (ch == '\r') { return; }
        else if (ch == '\n') { text.push_back('\n'); }
        else if (ch == '\t') { text.push_back('\t'); }
        else if (ch == 8)    { if (!text.empty()) text.pop_back(); }
        else if (ch >= 32 && ch < 127) { text.push_back((char)ch); }
        else { return; }

        if (text.size() > 8192) { text.erase(0, text.size() - 8192); }
        scrollDown = true;
    }

    std::string name;
    VFOManager::VFO* vfo;
    fldigi::PSKProfile profile;

    // DSP chain
    dsp::noise_reduction::PowerSquelch squelch;
    dsp::demod::SSB<float> ssb;
    dsp::demod::Quadrature nfm;
    fldigi::AutoTuner autoTune;
    fldigi::ToneMixer mixer;
    dsp::multirate::RationalResampler<dsp::complex_t> resamp;
    dsp::demod::PSK<2> psk;
    dsp::sink::Handler<dsp::complex_t> symSink;
    bool needResamp = false;

    // Decoding state. Plain BPSK modes use psk_varicode (PSK31-style).
    // BPSK63F uses mfsk_varicode and Viterbi K=7 FEC.
    fldigi::VaricodeDecoder      psk_varicode;
    fldigi::MfskVaricodeDecoder  mfsk_varicode;
    fldigi::ViterbiK7R12         viterbi;
    dsp::complex_t prev = { 1.0f, 0.0f };

    // GUI / output
    ImGui::SymbolDiagram diag;
    int diagIdx = 0;
    std::mutex textMtx;
    std::string text;
    bool scrollDown = false;

    // Settings / state
    VfoMode vfoMode = VFO_MODE_USB;
    double afFreq = 1000.0;
    bool running = false;
    bool nfmActive = false;

    // Squelch
    bool   squelchEnabled = false;
    float  squelchLevel   = -50.0f;  // dB
    float currentSquelchLevel() const {
        return squelchEnabled ? squelchLevel : PSK_SQUELCH_MIN;
    }
};
