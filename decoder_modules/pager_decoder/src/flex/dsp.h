#pragma once
#include <dsp/stream.h>
#include <dsp/sink/handler_sink.h>
#include <dsp/demod/quadrature.h>
#include <dsp/processor.h>
#include "flex.h"
#include <cstring>

// FLEX FM deviation (nominal). FLEX uses 4-level FSK with the outer
// levels at +/-4.8 kHz and the inner levels at +/-1.6 kHz.
#define FLEX_DEVIATION   4800.0

// FLEX DSP chain:
//
//   complex IQ  ->  Quadrature demod  ->  float samples  ->  flex::Decoder
//
// We deliberately do NOT add a matched filter or symbol clock recovery
// here - the FLEX decoder ported from multimon-ng does its own symbol
// timing, lock detection and AGC. We just need to produce FM-demodulated
// audio samples at a known rate (the rate passed to the flex::Decoder).
class FLEXDSP : public dsp::Processor<dsp::complex_t, float> {
    using base_type = dsp::Processor<dsp::complex_t, float>;

public:
    FLEXDSP() {}
    FLEXDSP(dsp::stream<dsp::complex_t>* in, double samplerate) {
        init(in, samplerate);
    }

    void init(dsp::stream<dsp::complex_t>* in, double samplerate) {
        _samplerate = samplerate;
        // Quadrature FM demodulator. Positive deviation here gives the
        // "natural" polarity expected by the FLEX symbol slicer.
        demod.init(NULL, FLEX_DEVIATION, _samplerate);
        base_type::init(in);
    }

    int process(int count, dsp::complex_t* in, float* out) {
        return demod.process(count, in, out);
    }

    int run() {
        int count = base_type::_in->read();
        if (count < 0) { return -1; }
        count = process(count, base_type::_in->readBuf, base_type::out.writeBuf);
        base_type::_in->flush();
        if (!base_type::out.swap(count)) { return -1; }
        return count;
    }

private:
    dsp::demod::Quadrature demod;
    double _samplerate = 0.0;
};
