#pragma once
#include "../decoder.h"
#include "modem.h"
#include <dsp/noise_reduction/power_squelch.h>
#include <dsp/demod/ssb.h>
#include <dsp/demod/fm.h>
#include <dsp/sink/handler_sink.h>
#include <algorithm>

// DominoEX decoder DSP chain (identical structure to the working thor_decoder):
//
//   VFO IQ -> PowerSquelch -> SSB(USB/LSB) or FM(NFM) -> Handler sink -> DominoEXModem
//
// CANONICAL SDR++ PATTERN (identical to m17 / radio / psk / mfsk / thor decoders):
//   * every DSP block is a MEMBER, init()'d EXACTLY ONCE in buildDSP() (called
//     from the constructor). init() registers a block's input/output streams;
//     calling it twice double-registers them and CRASHES SDR++.
//   * start()/stop() only ever call start()/stop()/setInput()/setMode() - NEVER
//     init(). Both demodulators are wired to the squelch output but only one is
//     ever started at a time (the stopped one's worker never reads the stream).

namespace dominoex {

    static constexpr double DOMINOEX_SSB_BW = 3000.0;   // SSB channel bandwidth (Hz)
    static constexpr double DOMINOEX_NFM_BW = 12500.0;  // NFM bandwidth (Hz)

    class DominoEXDecoder : public Decoder {
    public:
        DominoEXDecoder(dsp::stream<dsp::complex_t>* in) {
            vfoInput = in;
            modem.configure(modeIdx, afFreq);
            buildDSP();
        }
        ~DominoEXDecoder() { stop(); }

        void buildDSP() {
            squelch.init(vfoInput, currentSquelchLevel());
            ssb.init(&squelch.out, dsp::demod::SSB<float>::USB, DOMINOEX_SSB_BW, AUDIO_SR,
                     100.0 / AUDIO_SR, 5.0 / AUDIO_SR);
            nfm.init(&squelch.out, AUDIO_SR, DOMINOEX_NFM_BW, true);
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
            idx = std::clamp(idx, 0, DOMINOEX_MODE_COUNT - 1);
            if (idx == modeIdx) { return; }
            bool wasRunning = running;
            if (wasRunning) { stop(); }
            modeIdx = idx;
            modem.configure(modeIdx, afFreq);
            modem.setReverse(reverse);
            if (wasRunning) { start(); }
        }

        void setAFFreq(double freq) override { afFreq = freq; modem.setAFFreq(freq); }

        void setAFCEnabled(bool en) override { afcEnabled = en; modem.setAFC(en); }
        bool getAFCEnabled() const override { return afcEnabled; }
        double getTrackedAFFreq() const override { return modem.getTrackedAF(); }

        void setReverse(bool r) override { reverse = r; modem.setReverse(r); }
        bool getReverse() const override { return reverse; }

        int    getBandSpectrum(float* out, int n) const override { return modem.getBandSpectrum(out, n); }
        double getBandFlo() const override { return modem.getBandFlo(); }
        double getBandFhi() const override { return modem.getBandFhi(); }
        double getEdgeFreq() const override { return modem.getEdgeFreq(); }
        double getToneSpan() const override { return modem.getToneSpan(); }

        void setSquelchEnabled(bool en) override { squelchEnabled = en; squelch.setLevel(currentSquelchLevel()); }
        void setSquelchLevel(float dB) override {
            squelchLevel = std::clamp(dB, DOMINOEX_SQUELCH_MIN, DOMINOEX_SQUELCH_MAX);
            squelch.setLevel(currentSquelchLevel());
        }
        bool  getSquelchEnabled() const override { return squelchEnabled; }
        float getSquelchLevel()   const override { return squelchLevel; }

        int   getTone() const override { return modem.getTone(); }
        float getSNR()  const override { return modem.getSNR(); }

        void getConstellation(dsp::complex_t* buf, int n) {
            static std::complex<float> tmp[1024];
            int c = (n < 1024) ? n : 1024;
            modem.getScope(tmp, c);
            for (int i = 0; i < c; i++) { buf[i].re = tmp[i].real(); buf[i].im = tmp[i].imag(); }
        }

        void start() override {
            if (running) { return; }
            modem.configure(modeIdx, afFreq);
            modem.setReverse(reverse);
            squelch.setInput(vfoInput);
            squelch.setLevel(currentSquelchLevel());
            squelch.start();
            if (vfoMode == VFO_MODE_NFM) {
                nfm.setInput(&squelch.out);
                sink.setInput(&nfm.out);
                nfm.start();
            } else {
                ssb.setMode((vfoMode == VFO_MODE_LSB) ? dsp::demod::SSB<float>::LSB
                                                      : dsp::demod::SSB<float>::USB);
                ssb.setInput(&squelch.out);
                sink.setInput(&ssb.out);
                ssb.start();
            }
            sink.start();
            running = true;
        }

        void stop() override {
            if (!running) { return; }
            sink.stop();
            ssb.stop();
            nfm.stop();
            squelch.stop();
            running = false;
        }

    private:
        static void sinkHandler(float* data, int count, void* ctx) {
            auto* _this = (DominoEXDecoder*)ctx;
            _this->modem.process(data, count, [=](char c) {
                if (_this->onChar) { _this->onChar(c); }
            });
        }

        float currentSquelchLevel() const {
            return squelchEnabled ? squelchLevel : DOMINOEX_SQUELCH_MIN;
        }

        dsp::stream<dsp::complex_t>* vfoInput = nullptr;

        dsp::noise_reduction::PowerSquelch squelch;
        dsp::demod::SSB<float>             ssb;
        dsp::demod::FM<float>              nfm;
        dsp::sink::Handler<float>          sink;

        DominoEXModem modem;

        bool    running        = false;
        VfoMode vfoMode        = VFO_MODE_USB;
        int     modeIdx        = 4;        // default DominoEX 11 (index in DOMINOEX_MODES)
        double  afFreq         = 1000.0;
        bool    squelchEnabled = true;
        bool    afcEnabled     = false;
        bool    reverse        = false;
        float   squelchLevel   = -50.0f;
    };

}
