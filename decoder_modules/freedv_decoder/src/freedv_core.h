/*
 * freedv_core.h - FreeDV receive core wrapper around libcodec2's FreeDV API.
 *
 * This header deliberately depends ONLY on libcodec2 (freedv_api.h) and the
 * C++ standard library. It is shared verbatim between the SDR++ module (via
 * freedv_dsp.h) and the standalone test harness, so the exact same decode path
 * is exercised in both. No SDR++ headers are pulled in here.
 *
 * FreeDV over HF is an SSB mode: the modem audio is the audio you would hear
 * out of an SSB receiver. All of the modes handled here share a modem sample
 * rate of 8 kHz, so a single 8 kHz front-end feeds every mode. Only the
 * decoded speech sample rate differs (8 kHz for the Codec2 modes, 16 kHz for
 * the LPCNet-based 2020 modes), which is reported via getSpeechSampleRate().
 */
#pragma once

#include <codec2/freedv_api.h>

#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <cstring>
#include <algorithm>

namespace fdvmod {

// libcodec2 expects modem samples in 16-bit full-scale units. We carry audio
// internally as normalised floats (~ +/-1.0), so we scale by FREEDV_PEAK
// (the Tx modulator peak, 16384) to land at a safe mid-scale level before
// handing the samples to freedv_floatrx(). This mirrors how a real SSB rig's
// AGC presents the signal to freedv-gui.
#ifndef FDV_INPUT_SCALE
#define FDV_INPUT_SCALE 16384.0f
#endif

struct ModeInfo {
    int id;
    const char* name;
};

// HF SSB modes only (modem sample rate 8 kHz for all of them). The FM/FSK
// modes 2400A/2400B use a 48 kHz modem rate and a different (FM) front-end,
// so they are intentionally not part of this SSB decoder.
inline const ModeInfo* modeTable(int* count) {
    static const ModeInfo M[] = {
        { FREEDV_MODE_700D,  "700D"  },
        { FREEDV_MODE_700E,  "700E"  },
        { FREEDV_MODE_700C,  "700C"  },
        { FREEDV_MODE_1600,  "1600"  },
        { FREEDV_MODE_800XA, "800XA" },
        { FREEDV_MODE_2020,  "2020"  },
        { FREEDV_MODE_2020B, "2020B" },
    };
    if (count) { *count = (int)(sizeof(M) / sizeof(M[0])); }
    return M;
}

inline const char* modeName(int id) {
    int n = 0;
    const ModeInfo* m = modeTable(&n);
    for (int i = 0; i < n; i++) {
        if (m[i].id == id) { return m[i].name; }
    }
    return "?";
}

inline int modeIndex(int id) {
    int n = 0;
    const ModeInfo* m = modeTable(&n);
    for (int i = 0; i < n; i++) {
        if (m[i].id == id) { return i; }
    }
    return 0;
}

class FreeDVCore {
public:
    FreeDVCore() {}
    ~FreeDVCore() { close(); }

    // (Re)open the decoder for the given FREEDV_MODE_* id. Returns false if the
    // mode is unavailable in the linked libcodec2 build.
    bool open(int mode) {
        close();
        std::lock_guard<std::mutex> lck(mtx);
        fdv = freedv_open(mode);
        if (!fdv) { return false; }
        modeId = mode;
        modemFs = freedv_get_modem_sample_rate(fdv);
        speechFs = freedv_get_speech_sample_rate(fdv);
        int nSpeechMax = freedv_get_n_speech_samples(fdv);
        speechBuf.assign(nSpeechMax > 0 ? nSpeechMax : 4096, 0);
        accum.clear();
        readOff = 0;
        // Low-rate text channel: collected character by character.
        freedv_set_callback_txt(fdv, txtRxCb, nullptr, this);
        snrEst.store(0.0f);
        syncFlag.store(0);
        rxStatus.store(0);
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lck(mtx);
        if (fdv) {
            freedv_close(fdv);
            fdv = nullptr;
        }
    }

    int getModemSampleRate() const { return modemFs; }
    int getSpeechSampleRate() const { return speechFs; }
    int getModeId() const { return modeId; }

    // Feed n normalised float modem samples (~ +/-1.0). Any decoded speech
    // (normalised float at the speech sample rate) is appended to speechOut.
    void process(const float* in, int n, std::vector<float>& speechOut) {
        std::lock_guard<std::mutex> lck(mtx);
        if (!fdv || n <= 0) { return; }

        // Append the scaled input to the accumulator.
        size_t base = accum.size();
        accum.resize(base + (size_t)n);
        for (int i = 0; i < n; i++) {
            accum[base + (size_t)i] = in[i] * FDV_INPUT_SCALE;
        }

        // FreeDV asks for a varying number of samples per call (freedv_nin)
        // so the OFDM modem can track timing. Process every complete chunk.
        int nin = freedv_nin(fdv);
        while (nin > 0 && (int)(accum.size() - readOff) >= nin) {
            int nout = freedv_floatrx(fdv, speechBuf.data(), accum.data() + readOff);
            readOff += (size_t)nin;

            int sync = 0;
            float snr = 0.0f;
            freedv_get_modem_stats(fdv, &sync, &snr);
            snrEst.store(snr);
            int st = freedv_get_rx_status(fdv);
            rxStatus.store(st);
            syncFlag.store((st & FREEDV_RX_SYNC) ? 1 : 0);

            const float invShort = 1.0f / 32768.0f;
            for (int i = 0; i < nout; i++) {
                speechOut.push_back((float)speechBuf[i] * invShort);
            }
            nin = freedv_nin(fdv);
        }

        // Compact the accumulator once consumed samples build up.
        if (readOff > 0) {
            accum.erase(accum.begin(), accum.begin() + (long)readOff);
            readOff = 0;
        }
    }

    float snr() const { return snrEst.load(); }
    bool synced() const { return syncFlag.load() != 0; }
    int status() const { return rxStatus.load(); }

    std::string getText() {
        std::lock_guard<std::mutex> lck(txtMtx);
        return textBuf;
    }

    void clearText() {
        std::lock_guard<std::mutex> lck(txtMtx);
        textBuf.clear();
    }

private:
    static void txtRxCb(void* state, char c) {
        FreeDVCore* self = (FreeDVCore*)state;
        std::lock_guard<std::mutex> lck(self->txtMtx);
        self->textBuf.push_back(c);
        if (self->textBuf.size() > 4096) {
            self->textBuf.erase(0, self->textBuf.size() - 4096);
        }
    }

    struct freedv* fdv = nullptr;
    int modeId = -1;
    int modemFs = 8000;
    int speechFs = 8000;

    std::vector<float> accum;   // pending modem samples (scaled)
    size_t readOff = 0;         // consumed offset into accum
    std::vector<short> speechBuf;
    std::mutex mtx;

    std::atomic<float> snrEst{ 0.0f };
    std::atomic<int> syncFlag{ 0 };
    std::atomic<int> rxStatus{ 0 };

    std::mutex txtMtx;
    std::string textBuf;
};

} // namespace fdvmod
