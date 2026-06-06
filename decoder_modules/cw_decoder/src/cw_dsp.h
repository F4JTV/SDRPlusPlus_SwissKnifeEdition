#pragma once
#include <dsp/processor.h>
#include <dsp/loop/agc.h>
#include <dsp/filter/fir.h>
#include <dsp/taps/low_pass.h>
#include <volk/volk.h>

// Produces the magnitude envelope of the tuned channel. The VFO already
// band-limits the channel, so |z| tracks the CW keying. A light low-pass on
// the envelope removes the wideband noise that survives magnitude detection;
// the decoder applies its own adaptive bit-filter on top of this.
class CWDSP : public dsp::Processor<dsp::complex_t, float> {
    using base_type = dsp::Processor<dsp::complex_t, float>;
public:
    CWDSP() {}
    CWDSP(dsp::stream<dsp::complex_t>* in, double samplerate, double envBandwidth) {
        init(in, samplerate, envBandwidth);
    }

    ~CWDSP() {
        if (!base_type::_block_init) { return; }
        base_type::stop();
        dsp::taps::free(lpfTaps);
    }

    void init(dsp::stream<dsp::complex_t>* in, double samplerate, double envBandwidth) {
        _samplerate = samplerate;
        _envBw = envBandwidth;
        lpfTaps = dsp::taps::lowPass(_envBw, _envBw * 0.3, _samplerate);
        lpf.init(NULL, lpfTaps);
        lpf.out.free();
        base_type::init(in);
    }

    // Envelope low-pass cutoff. ~ keying bandwidth (tens of Hz).
    void setEnvBandwidth(double bw) {
        assert(base_type::_block_init);
        std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
        if (bw == _envBw) { return; }
        _envBw = bw;
        std::lock_guard<std::mutex> lck2(lpfMtx);
        dsp::taps::free(lpfTaps);
        lpfTaps = dsp::taps::lowPass(_envBw, _envBw * 0.3, _samplerate);
        lpf.setTaps(lpfTaps);
    }

    void setSamplerate(double samplerate) {
        assert(base_type::_block_init);
        std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
        base_type::tempStop();
        _samplerate = samplerate;
        std::lock_guard<std::mutex> lck2(lpfMtx);
        dsp::taps::free(lpfTaps);
        lpfTaps = dsp::taps::lowPass(_envBw, _envBw * 0.3, _samplerate);
        lpf.setTaps(lpfTaps);
        base_type::tempStart();
    }

    inline int process(int count, const dsp::complex_t* in, float* out) {
        // Magnitude (envelope) of the complex baseband.
        volk_32fc_magnitude_32f(out, (lv_32fc_t*)in, count);
        // Smooth the envelope.
        {
            std::lock_guard<std::mutex> lck(lpfMtx);
            lpf.process(count, out, out);
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
    double _samplerate = 8000.0;
    double _envBw = 50.0;
    dsp::tap<float> lpfTaps;
    dsp::filter::FIR<float, float> lpf;
    std::mutex lpfMtx;
};
