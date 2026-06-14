/*
 * js8gen.cpp - Standalone JS8 (Normal submode) signal generator.
 *
 * Renders a 12-character token to a 15 s, 12 kHz mono WAV file using the JS8
 * Normal-submode 8-FSK waveform, with optional additive white Gaussian noise
 * at a chosen SNR. Useful for exercising the decoder when no off-air sample is
 * available: the exact transmitted token is known, so decodes can be verified.
 *
 * Build (from this module's directory):
 *   g++ -std=c++17 -O2 -Isrc tools/js8gen.cpp src/js8_core.cpp -o js8gen
 *
 * Usage:
 *   js8gen <token12> <out.wav> [f0_hz=1500] [snr_db=99] [i3=0]
 *
 *   token12 : 12 characters over "0-9 A-Z a-z - +"
 *   snr_db  : audio SNR over the full 6 kHz band; 99 = no noise added
 *   i3      : frame type (0 heartbeat .. 3 directed, 4+ data)
 */
#include "js8_core.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

static void writeWav(const std::string& path, const std::vector<float>& x,
                     int sr) {
    std::vector<int16_t> pcm(x.size());
    for (size_t i = 0; i < x.size(); ++i) {
        float v = x[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        pcm[i] = (int16_t)std::lround(v * 32767.0f);
    }
    uint32_t dataBytes = (uint32_t)(pcm.size() * sizeof(int16_t));
    uint32_t byteRate = sr * 2;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); return; }
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); u32(36 + dataBytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16(1);
    u32((uint32_t)sr); u32(byteRate); u16(2); u16(16);
    fwrite("data", 1, 4, f); u32(dataBytes);
    fwrite(pcm.data(), 1, dataBytes, f);
    fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <token12> <out.wav> [f0_hz=1500] [snr_db=99] [i3=0]\n",
                argv[0]);
        return 1;
    }
    std::string token = argv[1];
    std::string out   = argv[2];
    float  f0    = (argc > 3) ? std::stof(argv[3]) : 1500.0f;
    double snrdb = (argc > 4) ? std::stod(argv[4]) : 99.0;
    int    i3    = (argc > 5) ? std::stoi(argv[5]) : 0;

    if (token.size() != 12) {
        fprintf(stderr, "token must be exactly 12 characters\n");
        return 1;
    }

    int tones[js8::NUM_SYMBOLS];
    try {
        js8::encodeNormal(token, i3, tones);
    } catch (const std::exception& e) {
        fprintf(stderr, "encode error: %s\n", e.what());
        return 1;
    }

    std::vector<float> audio = js8::genAudioNormal(tones, f0, 0.5f, true);

    if (snrdb < 90.0) {
        double sigP = 0.0; int cnt = 0;
        for (float v : audio) if (v != 0.0f) { sigP += (double)v * v; ++cnt; }
        if (cnt) sigP /= cnt;
        double noiseP = sigP / std::pow(10.0, snrdb / 10.0);
        std::mt19937 rng(std::random_device{}());
        std::normal_distribution<double> nd(0.0, std::sqrt(noiseP));
        for (float& v : audio) v += (float)nd(rng);
    }

    // Headroom.
    float pk = 0.0f;
    for (float v : audio) pk = std::max(pk, std::fabs(v));
    if (pk > 0.0f) for (float& v : audio) v *= (0.7f / pk);

    writeWav(out, audio, js8::SAMPLE_RATE);
    printf("wrote %s : token=%s f0=%.0f Hz snr=%.0f dB i3=%d (%zu samples)\n",
           out.c_str(), token.c_str(), f0, snrdb, i3, audio.size());
    return 0;
}
