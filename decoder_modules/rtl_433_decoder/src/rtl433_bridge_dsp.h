/** @file
    RTL433 bridge DSP block.

    Converts the SDR++ VFO's complex float IQ into CU8 (the native rtl_433
    sample format) and feeds it to the rtl_433 library wrapper. Decoded
    messages flow out through the wrapper's callback.

    Licensed under GPLv3 or later.
*/
#pragma once
#include <dsp/sink.h>
#include <vector>
#include <cstdint>
#include <algorithm>
#include "rtl433_lib.h"

class RTL433BridgeDSP : public dsp::Sink<dsp::complex_t> {
    using base_type = dsp::Sink<dsp::complex_t>;
public:
    RTL433BridgeDSP() {}

    void init(dsp::stream<dsp::complex_t>* in, uint32_t samplerate, uint32_t centerFreq,
              std::function<void(const rtl433br::Message&)> output) {
        _samplerate = samplerate;
        radio.registerAllProtocols(_includeDisabled);
        radio.configure(samplerate, centerFreq);
        radio.setLevels(_minLevelDb, _minSnrDb);
        radio.setOutput(output);
        cu8.resize(STREAM_BUFFER_SIZE * 2);
        base_type::init(in);
    }

    rtl433br::RTL433& getRadio() { return radio; }

    void setGain(float g) { _gain = g; }

    void setLevels(float minLevelDb, float minSnrDb) {
        _minLevelDb = minLevelDb;
        _minSnrDb = minSnrDb;
        radio.setLevels(minLevelDb, minSnrDb);
    }

    int run() override {
        int count = base_type::_in->read();
        if (count < 0) { return -1; }

        if ((int)cu8.size() < count * 2) { cu8.resize(count * 2); }

        dsp::complex_t* in = base_type::_in->readBuf;
        // SDR++ IQ is roughly in [-1, 1]; CU8 is unsigned 8-bit biased at 127.5.
        for (int i = 0; i < count; i++) {
            float fi = in[i].re * _gain * 127.0f + 127.5f;
            float fq = in[i].im * _gain * 127.0f + 127.5f;
            if (fi < 0.0f) fi = 0.0f; else if (fi > 255.0f) fi = 255.0f;
            if (fq < 0.0f) fq = 0.0f; else if (fq > 255.0f) fq = 255.0f;
            cu8[i * 2]     = (uint8_t)fi;
            cu8[i * 2 + 1] = (uint8_t)fq;
        }

        radio.feedCU8(cu8.data(), (uint32_t)(count * 2));

        base_type::_in->flush();
        return count;
    }

private:
    rtl433br::RTL433 radio;
    std::vector<uint8_t> cu8;
    float _gain = 1.0f;
    float _minLevelDb = -12.1442f;
    float _minSnrDb = 9.0f;
    bool _includeDisabled = false;
    uint32_t _samplerate = 250000;
};
