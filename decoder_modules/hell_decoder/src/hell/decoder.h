#pragma once
#include "../decoder.h"
#include "../feld/feld.h"
#include <dsp/noise_reduction/power_squelch.h>
#include <dsp/demod/ssb.h>
#include <dsp/demod/fm.h>
#include <dsp/sink/handler_sink.h>
#include <algorithm>

// Hell decoder DSP chain (same canonical structure as psk_decoder / mfsk_decoder):
//
//   VFO IQ -> PowerSquelch -> SSB(USB/LSB) or FM(NFM) -> Handler sink -> FeldModem
//
// The VFO is created at FELD_SAMPLE_RATE (8 kHz) so the audio arrives at the rate
// FLDIGI's feld modem expects natively; no extra resampling is needed.
//
// CANONICAL SDR++ PATTERN (identical to m17 / radio / psk / mfsk):
//   * every DSP block is a MEMBER, init()'d EXACTLY ONCE in buildDSP() (from the
//     constructor). init() registers a block's streams; calling it twice
//     double-registers and CRASHES SDR++.
//   * start()/stop() only call start()/stop()/setInput() - NEVER init(). Both
//     demodulators are wired to the squelch output; only one is started at a time.

namespace hell {

    static constexpr double AUDIO_SR    = FELD_SAMPLE_RATE; // 8 kHz
    static constexpr double HELL_SSB_BW = 3000.0;
    static constexpr double HELL_NFM_BW = 6000.0;

    class HellDecoder : public Decoder {
    public:
        HellDecoder(dsp::stream<dsp::complex_t>* in) {
            vfoInput = in;
            modem.configure(modeIdx, afFreq, rxHeight, agcMode, reverse, blackboard);
            buildDSP();
        }

        ~HellDecoder() { stop(); }

        void buildDSP() {
            squelch.init(vfoInput, currentSquelchLevel());

            ssb.init(&squelch.out, dsp::demod::SSB<float>::USB, HELL_SSB_BW, AUDIO_SR,
                     100.0 / AUDIO_SR, 5.0 / AUDIO_SR);
            nfm.init(&squelch.out, AUDIO_SR, HELL_NFM_BW, true);

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
            idx = std::clamp(idx, 0, HELL_MODE_COUNT - 1);
            modeIdx = idx;
            modem.configure(modeIdx, afFreq, rxHeight, agcMode, reverse, blackboard);
        }

        void setAFFreq(double freq) override { afFreq = freq; modem.setAFFreq(freq); }

        void setRxHeight(int px) override {
            rxHeight = std::clamp(px, 14, MAX_RX_COLUMN_LEN);
            modem.configure(modeIdx, afFreq, rxHeight, agcMode, reverse, blackboard);
        }
        void setAgcMode(int agc) override { agcMode = agc; modem.setAgcMode(agc); }
        void setReverse(bool en) override { reverse = en; modem.setReverse(en); }
        void setBlackboard(bool en) override { blackboard = en; modem.setBlackboard(en); }

        void setSquelchEnabled(bool en) override {
            squelchEnabled = en;
            squelch.setLevel(currentSquelchLevel());
            modem.setSquelch(en, 5.0); // FLDIGI metric squelch (0..100); off when !en
        }
        void setSquelchLevel(float dB) override {
            squelchLevel = std::clamp(dB, HELL_SQUELCH_MIN, HELL_SQUELCH_MAX);
            squelch.setLevel(currentSquelchLevel());
        }
        bool  getSquelchEnabled() const override { return squelchEnabled; }
        float getSquelchLevel()   const override { return squelchLevel; }

        int    getBandSpectrum(float* out, int n) override { return modem.getBandSpectrum(out, n); }
        double getBandFlo() const override { return 0.0; }
        double getBandFhi() const override { return 3500.0; }

        const uint8_t* getImageRGB() override { return modem.getImageRGB(); }
        int            getImageWidth() const override { return modem.getImageWidth(); }
        int            getImageHeight() const override { return modem.getImageHeight(); }
        int            getColumnsReceived() const override { return modem.getColumnsReceived(); }
        std::mutex&    getImageMutex() override { return modem.getImageMutex(); }
        void           clearImage() override { modem.clearImage(); }

        float getMetric() const override { return (float)modem.getMetric(); }

        void start() override {
            if (running) { return; }
            modem.configure(modeIdx, afFreq, rxHeight, agcMode, reverse, blackboard);

            squelch.setInput(vfoInput);
            squelch.setLevel(currentSquelchLevel());
            squelch.start();

            if (vfoMode == VFO_MODE_NFM) {
                nfm.setInput(&squelch.out);
                sink.setInput(&nfm.out);
                nfm.start();
            } else {
                ssb.setMode(vfoMode == VFO_MODE_USB ? dsp::demod::SSB<float>::USB
                                                    : dsp::demod::SSB<float>::LSB);
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
            HellDecoder* _this = (HellDecoder*)ctx;
            _this->modem.process(data, count);
        }

        float currentSquelchLevel() {
            // Off -> very low threshold so everything passes the RF squelch; the
            // FLDIGI metric squelch (in the modem) gates the actual image painting.
            return squelchEnabled ? squelchLevel : -1000.0f;
        }

        dsp::stream<dsp::complex_t>* vfoInput = nullptr;

        dsp::noise_reduction::PowerSquelch squelch;
        dsp::demod::SSB<float>             ssb;
        dsp::demod::FM<float>              nfm;
        dsp::sink::Handler<float>          sink;

        FeldModem modem;

        bool   running        = false;
        VfoMode vfoMode       = VFO_MODE_USB;
        int    modeIdx        = MODE_FELDHELL;
        double afFreq         = 1500.0;
        int    rxHeight       = 20;
        int    agcMode        = HELL_AGC_SLOW;
        bool   reverse        = false;
        bool   blackboard     = false;
        bool   squelchEnabled = true;
        float  squelchLevel   = -50.0f;
    };

}
