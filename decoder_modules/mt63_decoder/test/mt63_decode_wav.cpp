// Decode an MT63 WAV recording through the module's own modem wrapper
// (src/mt63/modem.h), with no SDR++ dependencies. This validates the exact
// code path the live module uses, just driven from a file instead of a VFO.
//
// The loader accepts any PCM/float WAV: it parses the chunk list, downmixes to
// mono and linear-resamples to the engine's 8 kHz, so a raw off-air recording
// (typically 44.1/48 kHz stereo) can be fed in directly.
//
//   build:  g++ -O2 -std=c++17 -I ../src ../src/mt63/jalocha/mt63base.cpp
//                ../src/mt63/jalocha/dsp.cpp mt63_decode_wav.cpp -o mt63_decode_wav
//   usage:  ./mt63_decode_wav file.wav [mode] [center]
//             mode    one of 500S 500L 1000S 1000L 2000S 2000L, or "scan" (default)
//             center  centre frequency in Hz, or "scan" (default) to sweep the passband
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include "mt63/modem.h"

using namespace mt63;

// ---- minimal but tolerant WAV reader ---------------------------------------
struct Wav { std::vector<float> mono; int rate = 0; };

static uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t* p) { return p[0] | p[1] << 8; }

static Wav loadWav(const char* path) {
    Wav w;
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open %s failed\n", path); return w; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> b(sz > 0 ? sz : 0);
    if (sz <= 12 || fread(b.data(), 1, sz, f) != (size_t)sz) { fclose(f); return w; }
    fclose(f);
    if (memcmp(b.data(), "RIFF", 4) || memcmp(b.data() + 8, "WAVE", 4)) {
        fprintf(stderr, "not a RIFF/WAVE file\n"); return w;
    }
    int channels = 1, bits = 16; uint16_t fmt = 1;
    size_t pos = 12;
    const uint8_t *data = nullptr; size_t dataLen = 0;
    while (pos + 8 <= b.size()) {
        const uint8_t* id = b.data() + pos;
        uint32_t clen = rd32(b.data() + pos + 4);
        const uint8_t* body = b.data() + pos + 8;
        if (!memcmp(id, "fmt ", 4) && clen >= 16) {
            fmt = rd16(body); channels = rd16(body + 2);
            w.rate = (int)rd32(body + 4); bits = rd16(body + 14);
        } else if (!memcmp(id, "data", 4)) {
            data = body; dataLen = std::min((size_t)clen, b.size() - (pos + 8));
        }
        pos += 8 + clen + (clen & 1);   // chunks are word-aligned
    }
    if (!data || w.rate <= 0) { fprintf(stderr, "no usable fmt/data chunk\n"); return w; }

    // decode interleaved samples -> mono float
    auto push = [&](double s, int& ch, std::vector<double>& acc) {
        acc.push_back(s);
        if (++ch == channels) { double m = 0; for (double v : acc) m += v; w.mono.push_back((float)(m / channels)); acc.clear(); ch = 0; }
    };
    std::vector<double> acc; int ch = 0;
    if (fmt == 3 && bits == 32) {                  // IEEE float
        size_t n = dataLen / 4;
        for (size_t i = 0; i < n; i++) { float s; memcpy(&s, data + i * 4, 4); push(s, ch, acc); }
    } else if (bits == 16) {
        size_t n = dataLen / 2;
        for (size_t i = 0; i < n; i++) { int16_t s = (int16_t)rd16(data + i * 2); push(s / 32768.0, ch, acc); }
    } else if (bits == 8) {
        for (size_t i = 0; i < dataLen; i++) push((data[i] - 128) / 128.0, ch, acc);
    } else if (bits == 32) {                       // PCM32
        size_t n = dataLen / 4;
        for (size_t i = 0; i < n; i++) { int32_t s = (int32_t)rd32(data + i * 4); push(s / 2147483648.0, ch, acc); }
    } else {
        fprintf(stderr, "unsupported sample format (fmt=%u bits=%d)\n", fmt, bits);
    }
    return w;
}

// crude linear resample to 8 kHz (the engine front-end filters anyway)
static std::vector<float> resample8k(const std::vector<float>& in, int srcRate) {
    if (srcRate == (int)MT63_SR || in.empty()) return in;
    double ratio = MT63_SR / srcRate;
    size_t outN = (size_t)(in.size() * ratio);
    std::vector<float> out(outN);
    for (size_t i = 0; i < outN; i++) {
        double x = i / ratio; size_t i0 = (size_t)x; double fr = x - i0;
        float a = in[i0], bb = (i0 + 1 < in.size()) ? in[i0 + 1] : a;
        out[i] = (float)(a + (bb - a) * fr);
    }
    return out;
}

static std::string sanitize(const std::string& s) {
    std::string o; for (char c : s) o.push_back((c >= 32 && c < 127) || c == '\n' || c == '\t' ? c : '.');
    return o;
}

static int modeIndex(const std::string& m) {
    for (int i = 0; i < MT63_MODE_COUNT; i++) {
        std::string n = MT63_MODES[i].name;             // "MT63-500S"
        if (m == n || m == n.substr(5)) return i;        // accept "500S" too
    }
    return -1;
}

// Decode the whole clip once at a fixed (mode, centre); return text + a score.
// maxSamples > 0 limits how much audio is processed (used to keep the blind
// scan fast: a few seconds is enough to tell whether a guess locks).
static std::string decodeOnce(const std::vector<float>& audio, int modeIdx, double center,
                              int& printable, float& snr, float& conf, size_t maxSamples = 0) {
    Mt63Modem modem;
    modem.configure(modeIdx, center, 32);
    std::string out;
    const int CHUNK = 512;
    size_t limit = (maxSamples > 0 && maxSamples < audio.size()) ? maxSamples : audio.size();
    for (size_t off = 0; off < limit; off += CHUNK) {
        int n = (int)std::min((size_t)CHUNK, limit - off);
        modem.process(audio.data() + off, n, [&](char c) { out.push_back(c); });
    }
    snr = modem.getSNR(); conf = modem.getConfidence();
    printable = 0; for (char c : out) if (c >= 32 && c < 127) printable++;
    return out;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: %s file.wav [mode|scan] [center|scan]\n", argv[0]); return 1; }
    std::string modeArg = argc > 2 ? argv[2] : "scan";
    std::string ctrArg  = argc > 3 ? argv[3] : "scan";

    Wav w = loadWav(argv[1]);
    if (w.mono.empty()) return 1;
    std::vector<float> audio = resample8k(w.mono, w.rate);
    printf("loaded %.2f s, src %d Hz -> %g Hz mono (%zu samples)\n",
           audio.size() / MT63_SR, w.rate, MT63_SR, audio.size());

    std::vector<int> modes;
    if (modeArg == "scan") for (int i = 0; i < MT63_MODE_COUNT; i++) modes.push_back(i);
    else { int mi = modeIndex(modeArg); if (mi < 0) { fprintf(stderr, "unknown mode %s\n", modeArg.c_str()); return 1; } modes.push_back(mi); }

    std::vector<double> centers;
    if (ctrArg == "scan") for (double c = 700; c <= 2100; c += 200.0) centers.push_back(c);
    else centers.push_back(atof(ctrArg.c_str()));

    bool scanning = (modes.size() * centers.size() > 1);
    int   bestMode = -1, bestPr = 0; double bestCenter = 0; float bestConf = -1;

    if (scanning) {
        // Phase 1: a short prefix is enough for the synchroniser to lock, which
        // pins down the centre and bandwidth fast (confidence locks long before
        // text appears). Short vs Long interleave both lock, so phase 1 cannot
        // tell them apart -- that is left to phase 2.
        size_t probeLen = (size_t)(14.0 * MT63_SR);
        for (int mi : modes) {
            for (double c : centers) {
                int pr; float snr, conf;
                decodeOnce(audio, mi, c, pr, snr, conf, probeLen);
                double score = conf * 1000.0 + pr;
                double bestScore = bestConf * 1000.0 + bestPr;
                if (bestMode < 0 || score > bestScore) {
                    bestMode = mi; bestCenter = c; bestPr = pr; bestConf = conf;
                }
            }
        }
        // Phase 2: at the winning centre, fully decode both interleave depths of
        // the winning bandwidth and keep whichever yields more printable text.
        int bw = MT63_MODES[bestMode].bw;
        int chosen = bestMode, chosenPr = -1;
        for (int mi = 0; mi < MT63_MODE_COUNT; mi++) {
            if (MT63_MODES[mi].bw != bw) continue;
            int pr; float snr, conf;
            decodeOnce(audio, mi, bestCenter, pr, snr, conf);
            if (pr > chosenPr) { chosen = mi; chosenPr = pr; }
        }
        bestMode = chosen;
    }

    int pr; float snr, conf;
    std::string best = decodeOnce(audio, bestMode, bestCenter, pr, snr, conf);
    printf("best: %s  centre=%.0f Hz  printable=%d  conf=%.2f  S/N=%.1f dB\n",
           MT63_MODES[bestMode].name, bestCenter, pr, conf, snr);
    printf("---- decoded ----\n%s\n----\n", sanitize(best).c_str());
    return 0;
}
