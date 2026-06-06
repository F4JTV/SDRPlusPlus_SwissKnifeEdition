#pragma once
#include "../decoder.h"
#include "../common/tone_mixer.h"
#include "../common/varicode.h"
#include "../common/viterbi_k5.h"
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

// Constants are shared with the BPSK decoder header but redefined here for
// safety (in case this is built independently). Identical values.
#ifndef PSK_AUDIO_SR
#define PSK_AUDIO_SR      24000.0
#define PSK_SSB_BW        2800.0
#define PSK_NFM_BW        12500.0
#define PSK_NFM_DEV       5000.0
#define PSK_DIAG_LEN      512
#define PSK_SQUELCH_MIN   -100.0f
#define PSK_SQUELCH_MAX   0.0f
#endif

namespace fldigi {

    // QPSK profile (always uses K=5 R=1/2 convolutional FEC)
    struct QPSKProfile {
        const char* name;
        double      baud;
        double      basebandSR;
        double      rrcBeta;
    };

    static inline const QPSKProfile& qpskProfile(int idx) {
        static const QPSKProfile profiles[] = {
            { "QPSK31",  31.25,  2000.0, 0.5 }, // 64 sps
            { "QPSK63",  62.5,   2000.0, 0.5 }, // 32 sps
            { "QPSK125", 125.0,  2000.0, 0.5 }, // 16 sps
            { "QPSK250", 250.0,  4000.0, 0.5 }, // 16 sps
            { "QPSK500", 500.0,  8000.0, 0.5 }, // 16 sps
        };
        return profiles[idx];
    }
    static constexpr int QPSK_PROFILE_COUNT = 5;

    // Same VFO geometry policy as BPSK decoder
    struct VfoGeometryQpsk {
        int    reference;
        double bandwidth;
    };

    static inline VfoGeometryQpsk vfoGeometryForQpsk(VfoMode mode) {
        switch (mode) {
        case VFO_MODE_USB: return { ImGui::WaterfallVFO::REF_LOWER,  PSK_SSB_BW };
        case VFO_MODE_LSB: return { ImGui::WaterfallVFO::REF_UPPER,  PSK_SSB_BW };
        case VFO_MODE_NFM: return { ImGui::WaterfallVFO::REF_CENTER, PSK_NFM_BW };
        }
        return { ImGui::WaterfallVFO::REF_LOWER, PSK_SSB_BW };
    }

}

class QPSKDecoder : public Decoder {
public:
    QPSKDecoder(const std::string& name, VFOManager::VFO* vfo, int profileIdx)
        : diag(1.0f, PSK_DIAG_LEN), profile(fldigi::qpskProfile(profileIdx))
    {
        this->name = name;
        this->vfo  = vfo;

        applyVfoGeometry(vfoMode);
        buildDSP();
    }

    ~QPSKDecoder() { stop(); }

    void showMenu() override {
        float menuWidth = ImGui::GetContentRegionAvail().x;

        // QPSK: scope shows the real part of recovered symbols. With the
        // -45° rotation we apply in differential detection, all four
        // constellation points project onto +/-1/sqrt(2) on the real axis,
        // so the scope still gives a useful lock indicator.
        ImGui::FillWidth();
        diag.draw();

        ImGui::TextUnformatted("Decoded text:");
        ImGui::BeginChild(("fldigi_qpsk_text_" + name).c_str(), ImVec2(menuWidth, 150.0f), true,
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

        if (ImGui::Button(("Clear##fldigi_qpsk_clear_" + name).c_str(), ImVec2(menuWidth, 0))) {
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

        // Reset decoder state on (re)start so mode changes don't inherit
        // stale Viterbi survivors or last symbol phase.
        prev = { 1.0f, 0.0f };
        viterbi.reset();
        varicode.reset();

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

    int    getBandSpectrum(float* out, int n) const override { return autoTune.getBandSpectrum(out, n); }
    double getBandFlo() const override { return autoTune.getBandFlo(); }
    double getBandFhi() const override { return autoTune.getBandFhi(); }
    double getSignalBandwidth() const override { return profile.baud; }
    float  getAFFreq() const override { return (float)afFreq; }

private:
    void applyVfoGeometry(VfoMode mode) {
        auto g = fldigi::vfoGeometryForQpsk(mode);
        vfo->setBandwidthLimits(100.0, 50000.0, false);
        vfo->setReference(g.reference);
        vfo->setSampleRate(PSK_AUDIO_SR, g.bandwidth);
        vfo->setBandwidth(g.bandwidth);
        vfo->setBandwidthLimits(g.bandwidth, g.bandwidth, true);
    }

    void buildDSP() {
        squelch.init(vfo->output, currentSquelchLevel());

        ssb.init(&squelch.out, dsp::demod::SSB<float>::USB, PSK_SSB_BW, PSK_AUDIO_SR,
                 100.0 / PSK_AUDIO_SR, 5.0 / PSK_AUDIO_SR);
        nfm.init(&squelch.out, PSK_NFM_DEV, PSK_AUDIO_SR);

        autoTune.init(&ssb.out, PSK_AUDIO_SR);

        mixer.init(&autoTune.out, afFreq, PSK_AUDIO_SR);

        needResamp = (profile.basebandSR < PSK_AUDIO_SR - 1.0);
        dsp::stream<dsp::complex_t>* pskIn = nullptr;
        if (needResamp) {
            resamp.init(&mixer.out, PSK_AUDIO_SR, profile.basebandSR);
            pskIn = &resamp.out;
        } else {
            pskIn = &mixer.out;
        }

        int sps = (int)(profile.basebandSR / profile.baud);
        int rrcTaps = 2 * sps + 1;

        // Costas<4> bandwidth: narrower than BPSK because we want to track
        // only slow carrier drift, not chase QPSK 90 degree data jumps.
        // For low baud rates the bandwidth (normalized to symbol rate)
        // needs to be quite small; for higher baud it can be larger since
        // each cycle of phase noise spans more symbols.
        double agcRate   = 1e-3;
        double costasBW  = 0.003;
        double omegaGain = 1e-6;
        double muGain    = 0.01;
        if (profile.baud >= 125.0) {
            costasBW  = 0.005;
            omegaGain = 1e-5;
            muGain    = 0.02;
        }
        if (profile.baud >= 500.0) {
            costasBW  = 0.01;
        }

        psk.init(pskIn, profile.baud, profile.basebandSR,
                 rrcTaps, profile.rrcBeta,
                 agcRate, costasBW, omegaGain, muGain, 0.01);

        symSink.init(&psk.out, symbolHandler, this);

        viterbi.reset();
    }

    static void symbolHandler(dsp::complex_t* data, int count, void* ctx) {
        QPSKDecoder* _this = (QPSKDecoder*)ctx;

        // Scope: real part of the received symbol
        {
            float* buf = _this->diag.acquireBuffer();
            for (int i = 0; i < count; i++) {
                buf[_this->diagIdx] = data[i].re;
                _this->diagIdx = (_this->diagIdx + 1) % PSK_DIAG_LEN;
            }
            _this->diag.releaseBuffer();
        }

        for (int i = 0; i < count; i++) {
            dsp::complex_t s = data[i];

            // Differential phase: d = current * conj(prev). The Costas<4>
            // upstream has aligned the constellation to (1,0)/(0,1)/(-1,0)/
            // (0,-1), so the differential phase is now cleanly at one of
            // {0, +pi/2, +pi, -pi/2} (modulo small noise).
            float dre = s.re * _this->prev.re + s.im * _this->prev.im;
            float dim = s.im * _this->prev.re - s.re * _this->prev.im;
            _this->prev = s;

            // Phase in [0, 2*PI). FLDIGI QPSK constellation at 0/90/180/270.
            float phase = std::atan2(dim, dre);
            if (phase < 0.0f) { phase += 2.0f * (float)M_PI; }

            // Quantise to nearest 90 degrees (FLDIGI psk.cxx line 1497).
            int bits = (int)(phase / ((float)M_PI / 2.0f) + 0.5f) & 3;

            // USB sideband transform (FLDIGI psk.cxx line 1194).
            bits = (4 - bits) & 3;

            // Feed two soft bits to Viterbi (FLDIGI psk.cxx line 1196):
            //   sym[0] = bits & 1
            //   sym[1] = NOT((bits >> 1) & 1)
            int s0 = bits & 1;
            int s1 = ((bits >> 1) & 1) ^ 1;

            int v;
            v = _this->viterbi.process(s0);
            if (v >= 0) {
                int c = _this->varicode.process(v);
                if (c >= 0) { _this->appendChar(c); }
            }
            v = _this->viterbi.process(s1);
            if (v >= 0) {
                int c = _this->varicode.process(v);
                if (c >= 0) { _this->appendChar(c); }
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
    fldigi::QPSKProfile profile;

    // DSP chain
    dsp::noise_reduction::PowerSquelch squelch;
    dsp::demod::SSB<float> ssb;
    dsp::demod::Quadrature nfm;
    fldigi::AutoTuner autoTune;
    fldigi::ToneMixer mixer;
    dsp::multirate::RationalResampler<dsp::complex_t> resamp;
    // Full QPSK demod chain: RRC + AGC + Costas<4> + M&M. The Costas<4>
    // uses the "4-fold symmetry" error function which has stable equilibria
    // at the four QPSK constellation points (1,0), (0,1), (-1,0), (0,-1).
    // Unlike Costas<2>, it does NOT cycle-slip on QPSK 90 degree data jumps
    // because the loop sees those jumps as movement BETWEEN equilibria.
    // After Costas lock, the M&M timing recovery sees a clean QPSK signal
    // and tracks symbol timing correctly. The differential decoder in
    // symbolHandler() then extracts the data bits from phase differences.
    dsp::demod::PSK<4> psk;
    dsp::sink::Handler<dsp::complex_t> symSink;
    bool needResamp = false;

    // Decoding state
    fldigi::VaricodeDecoder varicode;
    fldigi::ViterbiK5R12    viterbi;
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
    float  squelchLevel   = -50.0f;
    float currentSquelchLevel() const {
        return squelchEnabled ? squelchLevel : PSK_SQUELCH_MIN;
    }
};
