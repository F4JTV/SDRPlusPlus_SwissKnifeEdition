#pragma once
#include <cmath>
#include <dsp/processor.h>
#include <dsp/types.h>

namespace fldigi {

    // Translates a real audio stream down to complex baseband by mixing it with
    // a local NCO at -afFreq. The wanted tone (the FLDIGI cursor) ends up at DC;
    // the resulting image at 2*afFreq is removed by the downstream decimating
    // resampler (which filters at the *input* rate, so no aliasing onto DC).
    class ToneMixer : public dsp::Processor<float, dsp::complex_t> {
        using base_type = dsp::Processor<float, dsp::complex_t>;
    public:
        ToneMixer() {}

        void init(dsp::stream<float>* in, double freq, double samplerate) {
            _freq = freq;
            _samplerate = samplerate;
            phase = 0.0;
            recompute();
            base_type::init(in);
        }

        void setFreq(double freq) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _freq = freq;
            recompute();
        }

        void setSamplerate(double samplerate) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _samplerate = samplerate;
            recompute();
        }

        void reset() {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            phase = 0.0;
            base_type::tempStart();
        }

        inline int process(int count, const float* in, dsp::complex_t* out) {
            for (int i = 0; i < count; i++) {
                float c = cosf((float)phase);
                float s = sinf((float)phase);
                // audio * e^(-j*phase)  ->  tone at +freq moves to DC
                out[i].re = in[i] * c;
                out[i].im = in[i] * (-s);
                phase += dphase;
                if (phase >= 2.0 * M_PI) { phase -= 2.0 * M_PI; }
                else if (phase < 0.0)    { phase += 2.0 * M_PI; }
            }
            return count;
        }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }
            process(count, base_type::_in->readBuf, base_type::out.writeBuf);
            base_type::_in->flush();
            if (!base_type::out.swap(count)) { return -1; }
            return count;
        }

    private:
        void recompute() { dphase = 2.0 * M_PI * _freq / _samplerate; }

        double _freq = 1000.0;
        double _samplerate = 8000.0;
        double phase = 0.0;
        double dphase = 0.0;
    };

}
