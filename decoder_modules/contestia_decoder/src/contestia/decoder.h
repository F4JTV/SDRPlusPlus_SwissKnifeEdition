#pragma once
#include "../decoder.h"
#include "modem.h"
#include <dsp/noise_reduction/power_squelch.h>
#include <dsp/demod/ssb.h>
#include <dsp/sink/handler_sink.h>
#include <algorithm>

// Contestia decoder DSP chain (same structure as the proven mfsk_decoder):
//
//   VFO IQ (8 kHz) -> PowerSquelch -> SSB(USB/LSB) -> Handler sink -> ContestiaModem
//
// The VFO is created at 8 kHz so the SSB demodulator emits 8 kHz real audio,
// which is exactly what the Jalocha Contestia engine expects (its symbol/FFT
// lengths are defined relative to an 8 kHz rate). No extra resampler is needed.
//
// CANONICAL SDR++ PATTERN (identical to mfsk / psk / m17 / radio):
//   * every DSP block is a MEMBER, init()'d EXACTLY ONCE in buildDSP() (called
//     from the constructor). init() registers a block's streams; calling it
//     twice double-registers them and CRASHES SDR++.
//   * start()/stop() only ever call start()/stop()/setInput()/setMode() - never
//     init(). The modem is (re)configured only while the chain is stopped.

namespace contestia {

    static constexpr double CONTESTIA_SSB_BW = 3000.0;   // SSB channel bandwidth (Hz)

    class ContestiaDecoder : public Decoder {
    public:
        ContestiaDecoder(dsp::stream<dsp::complex_t>* in) {
            vfoInput = in;
            modem.configure(modeIdx, centerHz, syncMargin, syncInteg);
            buildDSP();
        }

        ~ContestiaDecoder() { stop(); }

        // --- one-time DSP construction (init each block exactly once) ---------
        void buildDSP() {
            squelch.init(vfoInput, currentSquelchLevel());
            ssb.init(&squelch.out, dsp::demod::SSB<float>::USB, CONTESTIA_SSB_BW, CONTESTIA_SR,
                     100.0 / CONTESTIA_SR, 5.0 / CONTESTIA_SR);
            sink.init(&ssb.out, sinkHandler, this);
        }

        void setInput(dsp::stream<dsp::complex_t>* in) override {
            bool wasRunning = running;
            if (wasRunning) { stop(); }
            vfoInput = in;
            squelch.setInput(vfoInput);
            if (wasRunning) { start(); }
        }

        void setVFOMode(VfoMode mode) override {
            if (mode == vfoMode) { return; }
            bool wasRunning = running;
            if (wasRunning) { stop(); }
            vfoMode = mode;
            if (wasRunning) { start(); }
        }

        void setMode(int idx) override {
            idx = std::clamp(idx, 0, CONTESTIA_MODE_COUNT - 1);
            if (idx == modeIdx) { return; }
            bool wasRunning = running;
            if (wasRunning) { stop(); }
            modeIdx = idx;
            modem.configure(modeIdx, centerHz, syncMargin, syncInteg);
            if (wasRunning) { start(); }
        }

        void setAFFreq(double freq) override {
            centerHz = freq;
            modem.setCenter(freq);
        }

        void setSyncMargin(int margin) override {
            margin = std::clamp(margin, 2, 16);
            if (margin == syncMargin) { return; }
            bool wasRunning = running;
            if (wasRunning) { stop(); }
            syncMargin = margin;
            modem.configure(modeIdx, centerHz, syncMargin, syncInteg);
            if (wasRunning) { start(); }
        }

        void setSyncInteg(int integ) override {
            integ = std::clamp(integ, 2, 8);
            if (integ == syncInteg) { return; }
            bool wasRunning = running;
            if (wasRunning) { stop(); }
            syncInteg = integ;
            modem.configure(modeIdx, centerHz, syncMargin, syncInteg);
            if (wasRunning) { start(); }
        }

        void set8bit(bool en) override { eightBit = en; modem.set8bit(en); }

        int    getBandSpectrum(float* out, int n) const override { return modem.getBandSpectrum(out, n); }
        double getBandFlo() const override { return modem.getBandFlo(); }
        double getBandFhi() const override { return modem.getBandFhi(); }
        double getEdgeFreq() const override { return modem.getEdgeFreq(); }
        double getToneSpan() const override { return modem.getToneSpan(); }
        double estimateCenter() const override { return modem.estimateCenter(); }

        void setSquelchEnabled(bool en) override {
            squelchEnabled = en;
            squelch.setLevel(currentSquelchLevel());
        }
        void setSquelchLevel(float dB) override {
            squelchLevel = std::clamp(dB, CONTESTIA_SQUELCH_MIN, CONTESTIA_SQUELCH_MAX);
            squelch.setLevel(currentSquelchLevel());
        }
        bool  getSquelchEnabled() const override { return squelchEnabled; }
        float getSquelchLevel()   const override { return squelchLevel; }

        float  getSNR()        const override { return modem.getSNR(); }
        float  getFreqOffset() const override { return modem.getFreqOffset(); }
        double getBaud()       const override { return modem.getBaud(); }

        // --- start/stop: NO init() here, only start/stop/setInput -------------
        void start() override {
            if (running) { return; }
            modem.configure(modeIdx, centerHz, syncMargin, syncInteg);
            modem.set8bit(eightBit);

            squelch.setInput(vfoInput);
            squelch.setLevel(currentSquelchLevel());
            squelch.start();

            ssb.setMode((vfoMode == VFO_MODE_LSB) ? dsp::demod::SSB<float>::LSB
                                                  : dsp::demod::SSB<float>::USB);
            ssb.setInput(&squelch.out);
            sink.setInput(&ssb.out);
            ssb.start();
            sink.start();
            running = true;
        }

        void stop() override {
            if (!running) { return; }
            sink.stop();
            ssb.stop();
            squelch.stop();
            running = false;
        }

    private:
        static void sinkHandler(float* data, int count, void* ctx) {
            auto* _this = (ContestiaDecoder*)ctx;
            _this->modem.process(data, count, [=](char c) {
                if (_this->onChar) { _this->onChar(c); }
            });
        }

        float currentSquelchLevel() const {
            return squelchEnabled ? squelchLevel : CONTESTIA_SQUELCH_MIN;
        }

        dsp::stream<dsp::complex_t>* vfoInput = nullptr;

        // DSP blocks (members, init'd once)
        dsp::noise_reduction::PowerSquelch squelch;
        dsp::demod::SSB<float>             ssb;
        dsp::sink::Handler<float>          sink;

        ContestiaModem modem;

        bool    running        = false;
        VfoMode vfoMode        = VFO_MODE_USB;
        int     modeIdx        = 11;      // default Cont16-500 (index in CONTESTIA_MODES)
        double  centerHz       = 1500.0;
        int     syncMargin     = 8;
        int     syncInteg      = 4;
        bool    eightBit       = false;
        bool    squelchEnabled = false;
        float   squelchLevel   = -50.0f;
    };

}
