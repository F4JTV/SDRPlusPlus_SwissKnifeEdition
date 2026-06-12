#pragma once
#include <dsp/processor.h>
#include <atomic>

// Gates the audio stream from the decoder's squelch decision. The gain is
// ramped (not switched) to avoid clicks. Driven by the same flag the decoder
// uses to gate decoding, so a single squelch setting controls both.
class AudioSquelch : public dsp::Processor<dsp::stereo_t, dsp::stereo_t> {
    using base_type = dsp::Processor<dsp::stereo_t, dsp::stereo_t>;
public:
    AudioSquelch() {}
    AudioSquelch(dsp::stream<dsp::stereo_t>* in, double samplerate) { init(in, samplerate); }

    void init(dsp::stream<dsp::stereo_t>* in, double samplerate) {
        setSamplerate(samplerate);
        base_type::init(in);
    }

    void setSamplerate(double samplerate) {
        // 5 ms attack, 20 ms release.
        stepUp = 1.0f / (float)(0.005 * samplerate);
        stepDn = 1.0f / (float)(0.020 * samplerate);
    }

    // Pointer to the decoder's squelch-open flag (true = audio passes).
    void setFlag(std::atomic<bool>* f) { flag = f; }

    inline int process(int count, const dsp::stereo_t* in, dsp::stereo_t* out) {
        float tgt = (!flag || flag->load(std::memory_order_relaxed)) ? 1.0f : 0.0f;
        for (int i = 0; i < count; i++) {
            if (gain < tgt) { gain += stepUp; if (gain > tgt) { gain = tgt; } }
            else if (gain > tgt) { gain -= stepDn; if (gain < tgt) { gain = tgt; } }
            out[i].l = in[i].l * gain;
            out[i].r = in[i].r * gain;
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
    std::atomic<bool>* flag = nullptr;
    float gain = 1.0f;
    float stepUp = 0.025f;
    float stepDn = 0.00625f;
};
