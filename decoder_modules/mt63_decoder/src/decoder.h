#pragma once
#include <dsp/stream.h>
#include <dsp/types.h>
#include <functional>

// Base interface for the MT63 decoder, mirroring the control surface of the
// olivia_decoder / mfsk_decoder / psk_decoder modules (VFO sideband, AF cursor,
// mode, squelch, start/stop) plus the MT63-specific integration depth.

namespace mt63 {

    enum VfoMode {
        VFO_MODE_USB = 0,
        VFO_MODE_LSB = 1
    };

    static constexpr float MT63_SQUELCH_MIN = -100.0f;
    static constexpr float MT63_SQUELCH_MAX = 0.0f;

    class Decoder {
    public:
        virtual ~Decoder() {}

        virtual void setInput(dsp::stream<dsp::complex_t>* in) = 0;

        virtual void setVFOMode(VfoMode mode) = 0;
        virtual void setMode(int modeIdx) = 0;       // index into MT63_MODES
        virtual void setAFFreq(double freq) = 0;     // signal CENTRE frequency (Hz)

        virtual void setIntegration(int integ) = 0;  // FEC integration depth (symbols)
        virtual void set8bit(bool en) = 0;

        virtual void  setSquelchEnabled(bool en) = 0;
        virtual void  setSquelchLevel(float dB) = 0;
        virtual bool  getSquelchEnabled() const = 0;
        virtual float getSquelchLevel()   const = 0;

        // GUI band display
        virtual int    getBandSpectrum(float* out, int n) const = 0;
        virtual double getBandFlo() const = 0;
        virtual double getBandFhi() const = 0;
        virtual double getEdgeFreq() const = 0;
        virtual double getToneSpan() const = 0;
        virtual double estimateCenter() const = 0;

        // Status read-outs for the GUI.
        virtual float  getSNR()        const = 0;
        virtual float  getFreqOffset() const = 0;
        virtual bool   getLock()       const = 0;
        virtual float  getConfidence() const = 0;
        virtual double getBaud()       const = 0;

        // Called once per decoded character.
        std::function<void(char)> onChar;

        virtual void start() = 0;
        virtual void stop() = 0;
    };

}
