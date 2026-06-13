// contestia_gen.cpp -- standalone Contestia test-signal generator.
//
// Encodes ASCII text into an Contestia audio WAV (8 kHz, mono, 16-bit) using the
// same Jalocha MFSK engine as the decoder, with an optional configurable noise
// level so the decoder can be exercised across a range of SNRs. Real off-air
// recordings (e.g. from the Signal Identification Wiki) decode too; this tool
// is for generating reproducible test vectors when on-air signals aren't handy.
//
// Build (from this directory):
//   g++ -O2 -std=c++17 -I ../src/contestia contestia_gen.cpp -o contestia_gen
//
// Examples:
//   ./contestia_gen --mode 16-500 "CQ CQ DE TEST 73"          -> contestia_16-500.wav
//   ./contestia_gen --mode 8-250 --snr 0 --center 1200 "..."  -> noisy 0 dB signal
//   ./contestia_gen --mode 32-1000 --out test.wav "hello"
//
//   --mode   T-B   tones-bandwidth (e.g. 4-125, 8-250, 16-500, 32-1000, 64-2000)
//   --center Hz    signal centre frequency in the audio band (default 1500)
//   --snr    dB    in-band SNR of added white gaussian noise (default: none)
//   --lead   sec   leading idle before text (default 1.0) for synchroniser lock
//   --out    file  output WAV path (default contestia_<mode>.wav)
//   --seed   N     RNG seed for the noise (default 1)
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <random>
#include <cmath>
#include "jalocha/pj_mfsk.h"

static const double SR = 8000.0;

static std::vector<double> encode(const std::string& text, int tones, int bw,
                                  double centerHz, double leadSec) {
    MFSK_Transmitter<double> Tx;
    Tx.bContestia = true;   // Contestia FEC/alphabet
    Tx.Tones = tones; Tx.Bandwidth = bw;
    Tx.SampleRate = SR; Tx.OutputSampleRate = SR; Tx.Reverse = 0;
    double fc = Tx.Bandwidth * (1.0 - 0.5 / Tx.Tones) / 2.0;
    Tx.FirstCarrierMultiplier = (centerHz - fc) / 500.0;
    if (Tx.Preset() < 0) { fprintf(stderr, "Tx.Preset failed\n"); return {}; }

    // number of idle (NUL) chars to cover the requested lead-in time
    double cps = (double)Tx.CharactersPerSecond();
    int lead = std::max(8, (int)std::lround(leadSec * cps));

    std::vector<double> audio;
    std::vector<double> buf(Tx.MaxOutputLen + 8);
    for (int i = 0; i < lead; i++) Tx.PutChar(0);
    for (char c : text) Tx.PutChar((uint8_t)c);
    for (int i = 0; i < lead; i++) Tx.PutChar(0);

    Tx.Start();
    bool stopping = false; int guard = 0;
    while (Tx.Running() && guard++ < 2000000) {
        if (Tx.GetReadReady() < Tx.BitsPerSymbol) {
            if (!stopping) { Tx.Stop(); stopping = true; }
        }
        int len = Tx.Output(buf.data());
        for (int i = 0; i < len; i++) audio.push_back(buf[i]);
    }
    return audio;
}

static void addNoise(std::vector<double>& a, double snrDb, unsigned seed) {
    if (a.empty()) return;
    double p = 0; for (double v : a) p += v * v; p /= a.size();
    if (p <= 0) return;
    double sigma = std::sqrt(p / std::pow(10.0, snrDb / 10.0));
    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0.0, sigma);
    for (double& v : a) v += nd(rng);
}

static void writeWav(const char* path, const std::vector<double>& a) {
    double peak = 1e-9; for (double v : a) peak = std::max(peak, std::fabs(v));
    double g = 0.9 / peak;
    std::vector<int16_t> s(a.size());
    for (size_t i = 0; i < a.size(); i++) {
        double x = a[i] * g; if (x > 1) x = 1; if (x < -1) x = -1;
        s[i] = (int16_t)std::lround(x * 32767.0);
    }
    uint32_t dataBytes = (uint32_t)(s.size() * 2);
    uint32_t sr = (uint32_t)SR;
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return; }
    auto w32 = [&](uint32_t v){ fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v){ fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); w32(36 + dataBytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16(1);
    w32(sr); w32(sr * 2); w16(2); w16(16);
    fwrite("data", 1, 4, f); w32(dataBytes);
    fwrite(s.data(), 2, s.size(), f);
    fclose(f);
    printf("wrote %s : %.2f s, %zu samples @ %g Hz\n", path, a.size() / SR, a.size(), SR);
}

int main(int argc, char** argv) {
    int tones = 16, bw = 500;
    double center = 1500.0, snr = 1e9, lead = 1.0;
    unsigned seed = 1;
    std::string out, text, modeStr = "16-500";
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--mode" && i + 1 < argc) { modeStr = argv[++i]; }
        else if (a == "--center" && i + 1 < argc) { center = atof(argv[++i]); }
        else if (a == "--snr"    && i + 1 < argc) { snr = atof(argv[++i]); }
        else if (a == "--lead"   && i + 1 < argc) { lead = atof(argv[++i]); }
        else if (a == "--out"    && i + 1 < argc) { out = argv[++i]; }
        else if (a == "--seed"   && i + 1 < argc) { seed = (unsigned)atoi(argv[++i]); }
        else { if (!text.empty()) text += " "; text += a; }
    }
    if (sscanf(modeStr.c_str(), "%d-%d", &tones, &bw) != 2) {
        fprintf(stderr, "bad --mode '%s' (expected tones-bandwidth, e.g. 16-500)\n", modeStr.c_str());
        return 1;
    }
    if (text.empty()) text = "CQ CQ DE TEST CONTESTIA 73";
    if (out.empty()) out = "contestia_" + modeStr + ".wav";

    printf("OL %d-%d  center=%.0f Hz  text=\"%s\"%s\n", tones, bw, center, text.c_str(),
           snr < 1e8 ? "" : "  (no noise)");
    auto a = encode(text, tones, bw, center, lead);
    if (a.empty()) return 1;
    if (snr < 1e8) { addNoise(a, snr, seed); printf("added noise: in-band SNR = %.1f dB\n", snr); }
    writeWav(out.c_str(), a);
    return 0;
}
