/*
 * js8test.cpp - JS8 (Normal submode) encode/decode roundtrip harness.
 *
 * Encodes a token, synthesises audio, optionally adds AWGN at a range of
 * SNRs, and checks that the decoder recovers the exact transmitted token.
 * This validates the encoder, the LDPC + CRC framing and the decoder
 * front-end without any external sample.
 *
 * Build (from this module's directory):
 *   g++ -std=c++17 -O2 -Isrc tools/js8test.cpp src/js8_core.cpp src/js8_varicode.cpp -o js8test
 *
 * Usage:
 *   js8test [token12=0A1B2C3D4E5F] [i3=0]
 */
#include "js8_core.h"
#include "js8_varicode.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::string token = (argc > 1) ? argv[1] : "0A1B2C3D4E5F";
    int i3 = (argc > 2) ? std::stoi(argv[2]) : 0;
    if (token.size() != 12) { fprintf(stderr, "token must be 12 chars\n"); return 1; }

    int tones[js8::NUM_SYMBOLS];
    try { js8::encodeNormal(token, i3, tones); }
    catch (const std::exception& e) { fprintf(stderr, "encode: %s\n", e.what()); return 1; }

    const float f0 = 1500.0f;
    std::vector<float> clean = js8::genAudioNormal(tones, f0, 0.5f, false);

    // Clean decode.
    auto res = js8::decodeNormal(clean.data(), clean.size(), 1400.0f, 1600.0f);
    bool cleanOk = false;
    for (const auto& d : res) if (d.token == token) cleanOk = true;
    printf("clean: %zu decode(s), exact token recovered = %s\n",
           res.size(), cleanOk ? "YES" : "NO");
    for (const auto& d : res) {
        printf("   token=%s i3=%d f0=%.0f dt=%+.2f snr=%.0f nerr=%d  msg='%s'\n",
               d.token.c_str(), d.i3, d.f0, d.dt, d.snr, d.harderrors,
               js8::interpretMessage(d.token, d.i3).c_str());
    }

    // AWGN sweep (narrow window for speed).
    double sigP = 0.0; int cnt = 0;
    for (float v : clean) if (v != 0.0f) { sigP += (double)v * v; ++cnt; }
    sigP /= cnt;
    std::mt19937 rng(2024);
    printf("\nAWGN sweep (audio SNR over full band):\n");
    for (double snrdb = 3; snrdb >= -24; snrdb -= 3) {
        int hits = 0; const int TR = 3;
        for (int tr = 0; tr < TR; ++tr) {
            double nP = sigP / std::pow(10.0, snrdb / 10.0);
            std::normal_distribution<double> nd(0.0, std::sqrt(nP));
            std::vector<float> a(clean.size());
            for (size_t i = 0; i < clean.size(); ++i) a[i] = clean[i] + (float)nd(rng);
            auto r = js8::decodeNormal(a.data(), a.size(), 1480.0f, 1520.0f);
            for (const auto& d : r) if (d.token == token) { ++hits; break; }
        }
        printf("  %+5.0f dB : %d/%d\n", snrdb, hits, TR);
    }
    return 0;
}
