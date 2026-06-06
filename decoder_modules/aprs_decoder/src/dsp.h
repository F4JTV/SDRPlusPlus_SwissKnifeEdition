#pragma once
#include <dsp/processor.h>
#include <dsp/demod/quadrature.h>
#include <dsp/correction/dc_blocker.h>

// ---------------------------------------------------------------------------
//  APRS DSP front-end
//
//  Takes the VFO's complex IQ baseband (already decimated to 24 kHz by the VFO)
//  and produces FM-demodulated real audio. The AFSK 1200 (Bell 202) tone
//  discrimination, bit recovery and HDLC framing are done downstream in
//  aprs::AFSK1200 (see aprs/afsk.h), fed by a handler sink on this output.
//
//  24 kHz output == exactly 20 samples per 1200-baud bit, which keeps the
//  correlator kernels short and the bit-sync clean.
// ---------------------------------------------------------------------------

class APRSDSP : public dsp::Processor<dsp::complex_t, float> {
    using base_type = dsp::Processor<dsp::complex_t, float>;
public:
    APRSDSP() {}
    APRSDSP(dsp::stream<dsp::complex_t>* in, double samplerate) { init(in, samplerate); }

    void init(dsp::stream<dsp::complex_t>* in, double samplerate) {
        _samplerate = samplerate;
        // FM discriminator. Deviation scaling is not critical for AFSK since the
        // tone correlator difference is sign/scale tolerant; ~5 kHz works well
        // for the typical narrow-band APRS channel.
        demod.init(NULL, 5000.0, samplerate);
        dcb.init(NULL, 0.01);
        demod.out.free();
        base_type::init(in);
    }

    int process(int count, dsp::complex_t* in, float* out) {
        count = demod.process(count, in, out);
        count = dcb.process(count, out, out); // remove residual DC offset
        return count;
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
    dsp::correction::DCBlocker<float> dcb;
    double _samplerate = 24000.0;
};
