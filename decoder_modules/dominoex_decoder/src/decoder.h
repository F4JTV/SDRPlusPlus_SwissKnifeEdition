#pragma once
#include <dsp/stream.h>
#include <dsp/types.h>
#include <functional>

// Base interface for the DominoEX decoder, mirroring the control surface of the
// thor_decoder / mfsk_decoder modules (VFO sideband, AF cursor, mode, squelch).

namespace dominoex {

    enum VfoMode {
        VFO_MODE_USB = 0,
        VFO_MODE_LSB = 1,
        VFO_MODE_NFM = 2
    };

    static constexpr float DOMINOEX_SQUELCH_MIN = -100.0f;
    static constexpr float DOMINOEX_SQUELCH_MAX = 0.0f;

    class Decoder {
    public:
        virtual ~Decoder() {}

        virtual void setInput(dsp::stream<dsp::complex_t>* in) = 0;

        virtual void setVFOMode(VfoMode mode) = 0;
        virtual void setMode(int modeIdx) = 0;       // index into DOMINOEX_MODES
        virtual void setAFFreq(double freq) = 0;

        virtual void  setSquelchEnabled(bool en) = 0;
        virtual void  setSquelchLevel(float dB) = 0;
        virtual bool  getSquelchEnabled() const = 0;
        virtual float getSquelchLevel()   const = 0;

        virtual void   setAFCEnabled(bool en) = 0;
        virtual bool   getAFCEnabled() const = 0;
        virtual double getTrackedAFFreq() const = 0;

        // GUI band display
        virtual int    getBandSpectrum(float* out, int n) const = 0;
        virtual double getBandFlo() const = 0;
        virtual double getBandFhi() const = 0;
        virtual double getEdgeFreq() const = 0;
        virtual double getToneSpan() const = 0;

        // Status read-outs for the GUI.
        virtual int   getTone() const = 0;
        virtual float getSNR()  const = 0;

        // Reverse (mirror) tone order — DominoEX supports a reversed spectrum.
        virtual void setReverse(bool r) = 0;
        virtual bool getReverse() const = 0;

        // Called once per decoded character.
        std::function<void(char)> onChar;

        virtual void start() = 0;
        virtual void stop() = 0;
    };

}
