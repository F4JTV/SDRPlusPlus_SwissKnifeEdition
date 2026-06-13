// mt63_gen.cpp -- MT63 signal generator.
//
// Encodes text into an MT63 WAV (8 kHz, mono, 16-bit) with an optional noise
// level, using Pawel Jalocha's MT63tx (the same engine the decoder uses). This
// is the generator to use when no on-air signal is available. It depends only
// on the vendored engine under ../src/mt63/jalocha, so no SDR++ build is needed.
//
//   g++ -O2 -std=c++17 -I ../src/mt63/jalocha mt63_gen.cpp
//       ../src/mt63/jalocha/mt63base.cpp ../src/mt63/jalocha/dsp.cpp -o mt63_gen
//
//   ./mt63_gen --mode 1000S "CQ CQ DE TEST 73"
//   ./mt63_gen --mode 2000L --snr 3 --center 1500 "EMCOMM NET CHECK-IN"
//   ./mt63_gen --mode 500L --out weak.wav --snr -2 "weak signal test"
//
// Options:
//   --mode <m>     500S 500L 1000S 1000L 2000S 2000L   (default 1000S)
//   --center <Hz>  signal centre frequency             (default 1500)
//   --snr <dB>     in-band SNR of added white noise; omit for a clean signal
//   --lead <sec>   idle lead-in for sync               (default 1.5)
//   --tail <sec>   idle tail to flush the interleaver  (default auto)
//   --out <file>   output WAV path                     (default mt63.wav)
//   --seed <N>     noise RNG seed                       (default 1)

#include "dsp.h"
#include "mt63base.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <random>

static constexpr double SR = 8000.0;

struct Mode { const char* name; int bw; int longIntlv; };
static const Mode MODES[] = {
    {"500S",  500, 0}, {"500L",  500, 1},
    {"1000S",1000, 0}, {"1000L",1000, 1},
    {"2000S",2000, 0}, {"2000L",2000, 1},
};
static int findMode(const std::string& s) {
    for (int i = 0; i < 6; i++) if (s == MODES[i].name) return i;
    return -1;
}

static bool writeWav(const std::string& path, const std::vector<double>& x) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    uint32_t n = (uint32_t)x.size();
    uint32_t dataBytes = n * 2;
    uint32_t riff = 36 + dataBytes;
    uint16_t fmt = 1, ch = 1, bps = 16, align = 2;
    uint32_t rate = (uint32_t)SR, byteRate = rate * align;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); uint32_t sz = 16; fwrite(&sz, 4, 1, f);
    fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&byteRate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&dataBytes, 4, 1, f);
    for (double v : x) {
        if (v > 1.0) v = 1.0;
        if (v < -1.0) v = -1.0;
        int16_t s = (int16_t)std::lround(v * 32767.0);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    return true;
}

int main(int argc, char** argv) {
    int modeIdx = 2;            // 1000S
    double center = 1500.0;
    double snr = 1e9;           // clean by default
    bool addNoise = false;
    double lead = -1.0, tail = -1.0;   // both auto by default, scaled to interleaver
    std::string out = "mt63.wav";
    uint32_t seed = 1;
    std::string text;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if      (a == "--mode")   { int m = findMode(next()); if (m >= 0) modeIdx = m; }
        else if (a == "--center") { center = atof(next().c_str()); }
        else if (a == "--snr")    { snr = atof(next().c_str()); addNoise = true; }
        else if (a == "--lead")   { lead = atof(next().c_str()); }
        else if (a == "--tail")   { tail = atof(next().c_str()); }
        else if (a == "--out")    { out = next(); }
        else if (a == "--seed")   { seed = (uint32_t)atoi(next().c_str()); }
        else                      { if (!text.empty()) text += ' '; text += a; }
    }
    // MT63 always loses the first several seconds of text to sync acquisition
    // and interleaver fill, so the canned default repeats enough to emerge.
    if (text.empty())
        text = "CQ CQ CQ DE MT63 TEST BEACON THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG "
               "0123456789 CQ CQ CQ DE MT63 TEST BEACON 73";

    const Mode& M = MODES[modeIdx];
    MT63tx tx;
    if (tx.Preset(center, M.bw, M.longIntlv)) { fprintf(stderr, "tx preset failed\n"); return 1; }

    std::vector<double> sig;
    auto pump = [&]() { for (int i = 0; i < tx.Comb.Output.Len; i++) sig.push_back(tx.Comb.Output.Data[i]); };

    tx.SendChar(0); int symSamples = tx.Comb.Output.Len; pump();
    // Auto lead-in: the receiver must acquire sync AND prime its de-interleaver
    // (DataInterleave symbols deep) before text appears, otherwise a short
    // message is swallowed by the acquisition transient. Give it the full
    // interleaver depth plus a generous sync-acquisition margin.
    int leadSyms = (lead < 0) ? (tx.DataInterleave + 64)
                              : std::max(1, (int)std::lround(lead * SR / std::max(1, symSamples)));
    for (int i = 0; i < leadSyms; i++) { tx.SendChar(0); pump(); }
    for (char c : text)               { tx.SendChar(c); pump(); }
    int tailSyms = (tail < 0) ? (tx.DataInterleave + 16)
                              : std::max(1, (int)std::lround(tail * SR / std::max(1, symSamples)));
    for (int i = 0; i < tailSyms; i++) { tx.SendChar(0); pump(); }

    // normalise to ~0.7 full-scale to leave headroom for noise
    double peak = 1e-9; for (double v : sig) peak = std::max(peak, std::fabs(v));
    for (double& v : sig) v *= 0.7 / peak;

    if (addNoise) {
        double sp = 0; for (double v : sig) sp += v * v; sp /= std::max<size_t>(1, sig.size());
        double lin = std::pow(10.0, snr / 10.0);
        double inBandFrac = M.bw / (SR / 2.0);     // white noise spread over full Nyquist
        double np = sp / lin / inBandFrac;
        std::mt19937 rng(seed);
        std::normal_distribution<double> g(0.0, std::sqrt(np));
        for (double& v : sig) v += g(rng);
        // re-normalise after noise so nothing clips
        peak = 1e-9; for (double v : sig) peak = std::max(peak, std::fabs(v));
        if (peak > 1.0) for (double& v : sig) v /= peak;
    }

    if (!writeWav(out, sig)) { fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }
    printf("Wrote %s : MT63-%s, center %.0f Hz, %.2f s%s, text=\"%s\"\n",
           out.c_str(), M.name, center, sig.size() / SR,
           addNoise ? (" , SNR " + std::to_string((int)snr) + " dB").c_str() : "", text.c_str());
    return 0;
}
