/*
 * freedv_dsp.h - SDR++ DSP block wrapping the FreeDV receive core.
 *
 * Input : real modem audio (float, normalised ~ +/-1.0) at 8 kHz, i.e. the
 *         audio recovered by the SSB (USB) demodulator.
 * Output: decoded speech as stereo samples at the mode's speech sample rate
 *         (8 kHz for the Codec2 modes, 16 kHz for the 2020 modes). The module
 *         resamples this to the sink rate.
 *
 * All decode work is delegated to fdvmod::FreeDVCore so the standalone test
 * harness exercises exactly the same path.
 */
#pragma once

#include <dsp/block.h>
#include <dsp/stream.h>
#include <dsp/types.h>

#include "freedv_core.h"

#include <vector>
#include <string>
#include <cassert>
#include <algorithm>

namespace fdvmod {

class FreeDVDSP : public dsp::block {
public:
    FreeDVDSP() {}
    FreeDVDSP(dsp::stream<float>* in, int mode) { init(in, mode); }

    ~FreeDVDSP() {
        if (!dsp::block::_block_init) { return; }
        dsp::block::stop();
    }

    void init(dsp::stream<float>* in, int mode) {
        _in = in;
        core.open(mode);
        dsp::block::registerInput(_in);
        dsp::block::registerOutput(&out);
        dsp::block::_block_init = true;
    }

    void setInput(dsp::stream<float>* in) {
        assert(dsp::block::_block_init);
        std::lock_guard<std::recursive_mutex> lck(dsp::block::ctrlMtx);
        dsp::block::tempStop();
        dsp::block::unregisterInput(_in);
        _in = in;
        dsp::block::registerInput(_in);
        dsp::block::tempStart();
    }

    // Switch FreeDV mode. Returns the new speech sample rate so the caller can
    // retune the downstream resampler.
    int setMode(int mode) {
        assert(dsp::block::_block_init);
        std::lock_guard<std::recursive_mutex> lck(dsp::block::ctrlMtx);
        dsp::block::tempStop();
        core.open(mode);
        dsp::block::tempStart();
        return core.getSpeechSampleRate();
    }

    int getSpeechSampleRate() { return core.getSpeechSampleRate(); }
    int getModemSampleRate() { return core.getModemSampleRate(); }
    float snr() { return core.snr(); }
    bool synced() { return core.synced(); }
    int status() { return core.status(); }
    std::string getText() { return core.getText(); }
    void clearText() { core.clearText(); }

    int run() {
        int count = _in->read();
        if (count < 0) { return -1; }

        speech.clear();
        core.process(_in->readBuf, count, speech);
        _in->flush();

        // Emit decoded speech as stereo. Output can span several FreeDV frames
        // per input read, so write in chunks that never exceed the stream
        // buffer (1 M samples); in practice a single swap is always enough.
        int produced = (int)speech.size();
        int off = 0;
        while (off < produced) {
            int chunk = std::min(produced - off, STREAM_BUFFER_SIZE);
            for (int i = 0; i < chunk; i++) {
                out.writeBuf[i].l = speech[off + i];
                out.writeBuf[i].r = speech[off + i];
            }
            if (!out.swap(chunk)) { return -1; }
            off += chunk;
        }
        return count;
    }

    dsp::stream<dsp::stereo_t> out;

private:
    dsp::stream<float>* _in = nullptr;
    FreeDVCore core;
    std::vector<float> speech;
};

} // namespace fdvmod
