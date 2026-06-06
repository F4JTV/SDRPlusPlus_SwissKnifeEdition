#pragma once
#include "../decoder.h"
#include "../common/tone_mixer.h"
#include "../common/mfsk_varicode.h"
#include "../common/auto_tune.h"

#include <string>
#include <mutex>
#include <cmath>
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

// Shared with bpsk/qpsk headers (re-declared here so this header is
// self-contained)
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

    // 8PSK profile. The basic 8PSK_125/250/500/1000 modes have NO FEC; the
    // 3 bits per symbol go straight into the Varicode decoder. (FLDIGI also
    // defines 8PSK*F/FL variants that add convolutional FEC and an interleaver;
    // these are not implemented here.)
    struct PSK8Profile {
        const char* name;
        double      baud;
        double      basebandSR;
        double      rrcBeta;
    };

    static inline const PSK8Profile& psk8Profile(int idx) {
        static const PSK8Profile profiles[] = {
            { "8PSK125",  125.0,  2000.0, 0.5 }, // 16 sps
            { "8PSK250",  250.0,  4000.0, 0.5 }, // 16 sps
            { "8PSK500",  500.0,  8000.0, 0.5 }, // 16 sps
            { "8PSK1000", 1000.0, 8000.0, 0.5 }, //  8 sps
        };
        return profiles[idx];
    }
    static constexpr int PSK8_PROFILE_COUNT = 4;

    // Same VFO geometry policy as the BPSK / QPSK decoders.
    struct VfoGeometryPsk8 {
        int    reference;
        double bandwidth;
    };

    static inline VfoGeometryPsk8 vfoGeometryForPsk8(VfoMode mode) {
        switch (mode) {
        case VFO_MODE_USB: return { ImGui::WaterfallVFO::REF_LOWER,  PSK_SSB_BW };
        case VFO_MODE_LSB: return { ImGui::WaterfallVFO::REF_UPPER,  PSK_SSB_BW };
        case VFO_MODE_NFM: return { ImGui::WaterfallVFO::REF_CENTER, PSK_NFM_BW };
        }
        return { ImGui::WaterfallVFO::REF_LOWER, PSK_SSB_BW };
    }

}

class PSK8Decoder : public Decoder {
public:
    PSK8Decoder(const std::string& name, VFOManager::VFO* vfo, int profileIdx)
        : diag(1.0f, PSK_DIAG_LEN), profile(fldigi::psk8Profile(profileIdx))
    {
        this->name = name;
        this->vfo  = vfo;

        applyVfoGeometry(vfoMode);
        buildDSP();
    }

    ~PSK8Decoder() { stop(); }

    void showMenu() override {
        float menuWidth = ImGui::GetContentRegionAvail().x;

        // Scope: real axis of recovered symbols. On 8PSK the 8 phase
        // positions project onto 5 values on the real axis
        // (+/-1, +/-sqrt(2)/2, 0). Less informative than for BPSK but still
        // shows when the demod has locked onto something coherent.
        ImGui::FillWidth();
        diag.draw();

        ImGui::TextUnformatted("Decoded text:");
        ImGui::BeginChild(("fldigi_psk8_text_" + name).c_str(), ImVec2(menuWidth, 150.0f), true,
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

        if (ImGui::Button(("Clear##fldigi_psk8_clear_" + name).c_str(), ImVec2(menuWidth, 0))) {
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

        // Reset decoder state on (re)start.
        prev = { 1.0f, 0.0f };
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
    // 8PSK occupied bandwidth scales with baud (roughly 1.5x baud for the
    // main lobe at typical RRC roll-off).
    double getSignalBandwidth() const override { return profile.baud * 1.5; }
    float  getAFFreq() const override { return (float)afFreq; }

private:
    void applyVfoGeometry(VfoMode mode) {
        auto g = fldigi::vfoGeometryForPsk8(mode);
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

        // Costas<8> bandwidth: even narrower than QPSK because 8PSK has
        // smaller angular separation (pi/4 between symbols) and is more
        // sensitive to phase noise. For high baud rates the bandwidth
        // can be wider since we span more cycles per second.
        double agcRate   = 1e-3;
        double costasBW  = 0.002;
        double omegaGain = 1e-6;
        double muGain    = 0.01;
        if (profile.baud >= 250.0) {
            costasBW  = 0.005;
            omegaGain = 1e-5;
            muGain    = 0.02;
        }
        if (profile.baud >= 1000.0) {
            costasBW  = 0.01;
        }

        psk.init(pskIn, profile.baud, profile.basebandSR,
                 rrcTaps, profile.rrcBeta,
                 agcRate, costasBW, omegaGain, muGain, 0.01);

        symSink.init(&psk.out, symbolHandler, this);
    }

    static void symbolHandler(dsp::complex_t* data, int count, void* ctx) {
        PSK8Decoder* _this = (PSK8Decoder*)ctx;

        // Scope: real axis of recovered symbols
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

            // Differential phase: d = current * conj(prev). After Costas<8>,
            // the constellation is locked to the 8 phase points 0, pi/4,
            // 2*pi/4, ..., 7*pi/4. Differential operation cancels the
            // (now-zero) absolute phase and exposes the per-symbol phase
            // change, which is what carries the data.
            float dre = s.re * _this->prev.re + s.im * _this->prev.im;
            float dim = s.im * _this->prev.re - s.re * _this->prev.im;
            _this->prev = s;

            // FLDIGI's plain (non-FEC) 8PSK convention (psk.cxx line 1491):
            //   phase -= M_PI;  // rotate the constellation by 180 degrees
            //   if (phase < 0) phase += 2*PI;
            //   bits = round(phase / (PI/4)) & 7;
            float phase = std::atan2(dim, dre) - (float)M_PI;
            if (phase < 0.0f) { phase += 2.0f * (float)M_PI; }

            int sym = (int)(phase / ((float)M_PI / 4.0f) + 0.5f) & 7;

            // Emit bits LSB first (FLDIGI psk.cxx line 1816-1820)
            for (int b = 0; b < 3; b++) {
                int bit = (sym >> b) & 1;
                int c = _this->varicode.process(bit);
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
    fldigi::PSK8Profile profile;

    // DSP chain
    dsp::noise_reduction::PowerSquelch squelch;
    dsp::demod::SSB<float> ssb;
    dsp::demod::Quadrature nfm;
    fldigi::AutoTuner autoTune;
    fldigi::ToneMixer mixer;
    dsp::multirate::RationalResampler<dsp::complex_t> resamp;
    // Full 8PSK demod chain: RRC + AGC + Costas<8> + M&M. Costas<8>
    // uses an error function with stable equilibria at all 8 PSK points,
    // so it does NOT cycle-slip on 45 degree data jumps. After Costas
    // lock, the M&M timing tracks symbol timing cleanly.
    dsp::demod::PSK<8> psk;
    dsp::sink::Handler<dsp::complex_t> symSink;
    bool needResamp = false;

    // Decoding state
    fldigi::MfskVaricodeDecoder varicode;
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
