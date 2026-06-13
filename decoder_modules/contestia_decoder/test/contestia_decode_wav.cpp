// Decode a real off-air Contestia WAV (mono, 8 kHz, 16-bit). Scans candidate
// centre frequencies and prints the best decode (most printable characters).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include "jalocha/pj_mfsk.h"

static const double SR = 8000.0;

static std::vector<double> loadWav(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open %s failed\n", path); return {}; }
    uint8_t hdr[44]; if (fread(hdr, 1, 44, f) != 44) { fclose(f); return {}; }
    // assume PCM16 mono 8k (we created it); read data chunk
    std::vector<double> out;
    int16_t s;
    while (fread(&s, 2, 1, f) == 1) out.push_back(s / 32768.0);
    fclose(f);
    return out;
}

static std::string decodeAt(const std::vector<double>& audio, int tones, int bw,
                            double centerHz, int& printable, double& snr) {
    MFSK_Receiver<double> Rx;
    Rx.bContestia = true;   // Contestia FEC/alphabet
    Rx.Tones = tones; Rx.Bandwidth = bw;
    Rx.SyncMargin = 8; Rx.SyncIntegLen = 4; Rx.SyncThreshold = 3.0;
    Rx.SampleRate = SR; Rx.InputSampleRate = SR; Rx.Reverse = 0;
    double fc_offset = Rx.Bandwidth * (1.0 - 0.5 / Rx.Tones) / 2.0;
    Rx.FirstCarrierMultiplier = (centerHz - fc_offset) / 500.0;
    if (Rx.Preset() < 0) return "";
    Rx.Reset();
    std::string out;
    const size_t CHUNK = 512; std::vector<double> c(CHUNK);
    for (size_t off = 0; off < audio.size(); off += CHUNK) {
        size_t n = std::min(CHUNK, audio.size() - off);
        for (size_t i = 0; i < n; i++) c[i] = audio[off + i];
        Rx.Process(c.data(), n);
        uint8_t ch; while (Rx.GetChar(ch) > 0) if (ch >= 7) out.push_back((char)ch);
    }
    Rx.Flush();
    { uint8_t ch; while (Rx.GetChar(ch) > 0) if (ch >= 7) out.push_back((char)ch); }
    snr = Rx.SignalToNoiseRatio();
    // Score the lock by *text-likeness*, not raw printable count: a wrong
    // centre still produces plenty of printable junk (mostly punctuation), so
    // counting printables alone picks false peaks. Real text is dominated by
    // letters and spaces, so weight those and penalise punctuation.
    printable = 0;
    for (char ch : out) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == ' ')
            printable += 2;
        else if (ch >= '0' && ch <= '9')
            printable += 1;
        else if (ch >= 33 && ch < 127)
            printable -= 1;   // punctuation: likely a mis-lock artefact
    }
    return out;
}

static std::string sanitize(const std::string& s) {
    std::string o; for (char c : s) o.push_back((c >= 32 && c < 127) || c == '\n' ? c : '.');
    return o;
}

int main(int argc, char** argv) {
    if (argc < 4) { printf("usage: %s file.wav tones bw\n", argv[0]); return 1; }
    int tones = atoi(argv[2]), bw = atoi(argv[3]);
    auto audio = loadWav(argv[1]);
    if (audio.empty()) return 1;
    printf("loaded %.2f s @ %g Hz, mode OL %d-%d\n", audio.size()/SR, SR, tones, bw);

    double bestScore = -1e9, bestCenter = 0; std::string best;
    // scan centre across the SSB passband (fine step: tones can be 15 Hz apart)
    for (double center = 500; center <= 2200; center += 10.0) {
        int pr; double snr;
        std::string txt = decodeAt(audio, tones, bw, center, pr, snr);
        double score = pr;                // text-likeness score dominates
        if (score > bestScore) { bestScore = score; bestCenter = center; best = txt; }
    }
    printf("best centre = %.0f Hz\n", bestCenter);
    printf("---- decoded ----\n%s\n----\n", sanitize(best).c_str());
    return 0;
}
