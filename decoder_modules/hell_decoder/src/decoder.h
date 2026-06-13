#pragma once
#include <dsp/stream.h>
#include <dsp/types.h>

// Base control surface for the Hell decoder front-end, mirroring the
// mfsk/rtty/psk modules (VFO sideband, AF cursor, mode, squelch, start/stop)
// plus the Hell-specific display knobs (RX height, AGC, reverse, blackboard).

namespace hell {

    enum VfoMode {
        VFO_MODE_USB = 0,
        VFO_MODE_LSB = 1,
        VFO_MODE_NFM = 2
    };

    static constexpr float HELL_SQUELCH_MIN = -100.0f;
    static constexpr float HELL_SQUELCH_MAX = 0.0f;

    class Decoder {
    public:
        virtual ~Decoder() {}

        virtual void setInput(dsp::stream<dsp::complex_t>* in) = 0;

        virtual void setVFOMode(VfoMode mode) = 0;
        virtual void setMode(int modeIdx) = 0;       // index into HELL_MODES
        virtual void setAFFreq(double freq) = 0;
        virtual void setRxHeight(int px) = 0;
        virtual void setAgcMode(int agc) = 0;
        virtual void setReverse(bool en) = 0;
        virtual void setBlackboard(bool en) = 0;

        virtual void  setSquelchEnabled(bool en) = 0;
        virtual void  setSquelchLevel(float dB) = 0;
        virtual bool  getSquelchEnabled() const = 0;
        virtual float getSquelchLevel()   const = 0;

        // GUI band display
        virtual int    getBandSpectrum(float* out, int n) = 0;
        virtual double getBandFlo() const = 0;
        virtual double getBandFhi() const = 0;

        // Image access for the GL texture upload.
        virtual const uint8_t* getImageRGB() = 0;
        virtual int            getImageWidth() const = 0;
        virtual int            getImageHeight() const = 0;
        virtual int            getColumnsReceived() const = 0;
        virtual std::mutex&    getImageMutex() = 0;
        virtual void           clearImage() = 0;

        virtual float getMetric() const = 0;

        virtual void start() = 0;
        virtual void stop() = 0;
    };

}
