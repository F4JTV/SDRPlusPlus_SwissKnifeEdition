// Standalone DominoEX loopback test harness (no SDR++ runtime required).
//
//   generator (48 kHz) -> + AWGN -> DominoEXModem::process -> decoded text
//
// Verifies the decoder against the generator at several SNRs, for one or all
// modes. Also writes the clean signal to a WAV for inspection.
#include "dominoex/modem.h"
#include "dominoex/generator.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <random>
#include <string>
#include <vector>
#include <algorithm>

using namespace dominoex;

static double rmsOf(const std::vector<float>& x) {
    double s = 0; for (float v : x) s += (double)v * v;
    return std::sqrt(s / std::max((size_t)1, x.size()));
}

static std::vector<float> addNoise(const std::vector<float>& x, double snrDb, std::mt19937& rng) {
    double rms = rmsOf(x);
    double nstd = rms / std::pow(10.0, snrDb / 20.0);
    std::normal_distribution<double> nd(0.0, nstd);
    std::vector<float> y(x.size());
    for (size_t i = 0; i < x.size(); i++) y[i] = (float)(x[i] + nd(rng));
    return y;
}

// longest common substring length (rough decode-quality measure)
static int lcsSub(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return 0;
    std::vector<int> prev(b.size() + 1, 0), cur(b.size() + 1, 0);
    int best = 0;
    for (size_t i = 1; i <= a.size(); i++) {
        for (size_t j = 1; j <= b.size(); j++) {
            if (a[i - 1] == b[j - 1]) { cur[j] = prev[j - 1] + 1; best = std::max(best, cur[j]); }
            else cur[j] = 0;
        }
        std::swap(prev, cur);
    }
    return best;
}

static std::string decode(int modeIdx, double af, const std::vector<float>& audio) {
    DominoEXModem modem;
    modem.configure(modeIdx, af);
    std::string got;
    const int CH = 4096;
    for (size_t off = 0; off < audio.size(); off += CH) {
        int n = (int)std::min((size_t)CH, audio.size() - off);
        modem.process(audio.data() + off, n, [&](char c) { got.push_back(c); });
    }
    return got;
}

static void writeWav(const char* path, const std::vector<float>& x, int sr) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    int32_t nb = (int32_t)x.size() * 2;
    auto w32 = [&](uint32_t v){ fwrite(&v,4,1,f); };
    auto w16 = [&](uint16_t v){ fwrite(&v,2,1,f); };
    fwrite("RIFF",1,4,f); w32(36 + nb); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); w32(16); w16(1); w16(1); w32(sr); w32(sr*2); w16(2); w16(16);
    fwrite("data",1,4,f); w32(nb);
    for (float v : x) { int s = (int)std::lround(std::clamp(v, -1.0f, 1.0f) * 30000.0f); w16((int16_t)s); }
    fclose(f);
}

int main(int argc, char** argv) {
    std::string msg = "CQ CQ DE TEST the quick brown fox jumps over the lazy dog 0123456789";
    double af = 1500.0;
    std::mt19937 rng(12345);

    int onlyMode = -1;
    if (argc > 1) onlyMode = atoi(argv[1]);

    double snrs[] = { 99.0, 6.0, 3.0, 0.0, -3.0, -6.0, -9.0 };

    printf("DominoEX loopback test  (message %zu chars, AF %.0f Hz)\n", msg.size(), af);
    printf("%-12s", "mode");
    for (double s : snrs) { if (s > 90) printf("  %6s", "clean"); else printf("  %5.0fdB", s); }
    printf("\n");

    int passClean = 0, total = 0;
    for (int m = 0; m < DOMINOEX_MODE_COUNT; m++) {
        if (onlyMode >= 0 && m != onlyMode) continue;
        DominoEXGenerator gen(m, af);
        std::vector<float> clean = gen.generate(msg);
        if (m == 4) writeWav("/home/claude/work/out/dominoex11_clean.wav", clean, (int)AUDIO_SR);

        printf("%-12s", DOMINOEX_MODES[m].name);
        bool first = true;
        for (double snr : snrs) {
            std::vector<float> audio = (snr > 90) ? clean : addNoise(clean, snr, rng);
            std::string got = decode(m, af, audio);
            int L = lcsSub(msg, got);
            double frac = (double)L / msg.size();
            printf("  %5.0f%%", frac * 100.0);
            if (first) { total++; if (frac >= 0.9) passClean++; first = false; }
        }
        printf("   [clean got: \"");
        std::string g = decode(m, af, clean);
        // print a trimmed view of the clean decode
        std::string show; for (char c : g) { if (c >= 32 && c < 127) show.push_back(c); }
        size_t p = show.find("CQ");
        if (p != std::string::npos) show = show.substr(p, msg.size());
        printf("%s\"]\n", show.c_str());
    }
    printf("\nclean-decode pass (>=90%% LCS): %d/%d modes\n", passClean, total);
    return 0;
}
