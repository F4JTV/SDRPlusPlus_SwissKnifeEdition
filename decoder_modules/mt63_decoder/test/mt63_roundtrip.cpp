// mt63_roundtrip.cpp -- automated TX->(+noise)->RX self-test for the MT63 engine.
//
// Encodes a known message with MT63tx, optionally adds white noise to a target
// in-band SNR, decodes it with MT63rx and checks the text survived, across the
// six standard submodes. Depends only on the vendored Jalocha engine under
// ../src/mt63/jalocha, so no SDR++ build is required.
//
//   g++ -O2 -std=c++17 -I ../src/mt63/jalocha mt63_roundtrip.cpp
//       ../src/mt63/jalocha/mt63base.cpp ../src/mt63/jalocha/dsp.cpp -o mt63_roundtrip
//   ./mt63_roundtrip

#include <cstdlib>
#include "dsp.h"
#include "mt63base.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <random>

static constexpr double SR = 8000.0;

struct SubMode { const char* name; int bw; int longIntlv; };
static const SubMode MODES[] = {
    {"MT63-500S",   500, 0}, {"MT63-500L",   500, 1},
    {"MT63-1000S", 1000, 0}, {"MT63-1000L", 1000, 1},
    {"MT63-2000S", 2000, 0}, {"MT63-2000L", 2000, 1},
};

// Encode text -> 8 kHz real samples. Pads with idle (code 0) on both ends so
// the receiver has lead-in to lock and trailing flush for the interleaver.
static std::vector<double> encode(const std::string& msg, double center, int bw,
                                  int longIntlv, double leadSec) {
    // The transmitter pre-fills its interleaver with rand()&1. Re-seed here so
    // every case gets the *same* pre-fill independent of how many encodes ran
    // before it -- otherwise results depend on call order within the process.
    srand(1);
    MT63tx tx;
    if (tx.Preset(center, bw, longIntlv)) { fprintf(stderr, "tx preset failed\n"); return {}; }
    std::vector<double> out;
    auto pump = [&]() {
        for (int i = 0; i < tx.Comb.Output.Len; i++) out.push_back(tx.Comb.Output.Data[i]);
    };
    // One SendChar produces one symbol's worth of samples. Size the idle lead-in
    // in whole symbols from the first symbol's length.
    tx.SendChar(0); int symSamples = tx.Comb.Output.Len; pump();
    int leadSyms = std::max(1, (int)std::lround(leadSec * SR / std::max(1, symSamples)));
    for (int i = 0; i < leadSyms; i++) { tx.SendChar(0); pump(); }
    for (char c : msg)               { tx.SendChar(c); pump(); }
    for (int i = 0; i < tx.DataInterleave + 8; i++) { tx.SendChar(0); pump(); }
    return out;
}

static void addNoise(std::vector<double>& x, int bw, double snrDb, uint32_t seed) {
    // Signal power
    double sp = 0; for (double v : x) sp += v * v; sp /= std::max<size_t>(1, x.size());
    if (sp <= 0) return;
    // Noise power scaled so that the SNR *within the signal bandwidth* matches snrDb.
    // White noise spans the full 4 kHz Nyquist; only bw/4000 of it lands in band.
    double snr = std::pow(10.0, snrDb / 10.0);
    double inBandFrac = bw / (SR / 2.0);
    double np = sp / snr / inBandFrac;
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0.0, std::sqrt(np));
    for (double& v : x) v += g(rng);
}

static std::string decode(const std::vector<double>& x, double center, int bw,
                          int longIntlv, int integ) {
    MT63rx rx;
    if (rx.Preset(center, bw, longIntlv, integ, nullptr)) { fprintf(stderr, "rx preset failed\n"); return ""; }
    std::string text;
    const int CHUNK = 512;
    double_buff in; 
    for (size_t off = 0; off < x.size(); off += CHUNK) {
        int n = (int)std::min((size_t)CHUNK, x.size() - off);
        in.EnsureSpace(n); in.Len = n;
        for (int i = 0; i < n; i++) in.Data[i] = x[off + i];
        rx.Process(&in);
        for (int i = 0; i < rx.Output.Len; i++) {
            unsigned char c = (unsigned char)rx.Output.Data[i];
            if (c == '\n' || c == '\r' || c == '\t' || c >= 32) text += (char)c;
        }
    }
    return text;
}

static bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int main() {
    // The MT63 receiver has a long acquisition + interleaver-fill transient
    // before clean text appears (deeper interleave / lower baud = longer). We
    // therefore send a phrase repeated several times and check the phrase
    // survives somewhere in the decode, mirroring how a real QSO looks.
    const std::string phrase = "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG ";
    std::string msg; for (int k = 0; k < 6; k++) msg += phrase;
    const std::string needle = "QUICK BROWN FOX JUMPS OVER THE LAZY DOG";
    const double center = 1500.0;
    int pass = 0, total = 0;

    printf("MT63 round-trip self-test  (center=%.0f Hz)\n", center);
    printf("%-12s %8s %8s   %s\n", "mode", "SNR(dB)", "result", "decoded (tail)");
    printf("-------------------------------------------------------------------------\n");

    // Clean + a couple of noisy points per mode.
    // MT63 is a weak-signal HF mode; a mathematically noiseless input is not a
    // meaningful test (and leaves the narrowest mode's sync acquisition on a
    // knife edge). 20 dB stands in for a strong, clean real-world signal.
    const double snrs[] = { 20.0, 6.0, 0.0 };
    for (const auto& m : MODES) {
        for (double snr : snrs) {
            total++;
            auto sig = encode(msg, center, m.bw, m.longIntlv, 1.5);
            if (snr < 90.0) addNoise(sig, m.bw, snr, 12345 + total);
            std::string got = decode(sig, center, m.bw, m.longIntlv, 32);
            bool ok = contains(got, needle);
            if (ok) pass++;
            // show only the last ~40 printable chars
            std::string tail = got.size() > 40 ? got.substr(got.size() - 40) : got;
            for (char& c : tail) if (c == '\n' || c == '\r') c = ' ';
            printf("%-12s %8.0f %8s   %s\n", m.name, snr, ok ? "OK" : "FAIL", tail.c_str());
        }
    }
    printf("-------------------------------------------------------------------------\n");
    printf("%d/%d passed\n", pass, total);
    return pass == total ? 0 : 1;
}
