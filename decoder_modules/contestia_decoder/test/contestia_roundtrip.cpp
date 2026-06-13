// Standalone Contestia round-trip validation harness for the Jalocha MFSK engine.
//   encode(text) -> audio @ 8 kHz -> [+ AWGN] -> decode -> text
// Used to prove the vendored pj_mfsk.h decoder works before wrapping it in SDR++.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <random>
#include <cmath>
#include "jalocha/pj_mfsk.h"

static const double SR = 8000.0;

// Encode an ASCII string to Contestia audio (8 kHz, mono, normalised ~[-1,1]).
static std::vector<double> encode(const std::string& text, int tones, int bw,
                                  double centerHz) {
    MFSK_Transmitter<double> Tx;
    Tx.bContestia       = true;   // Contestia FEC/alphabet
    Tx.Tones            = tones;
    Tx.Bandwidth        = bw;
    Tx.SampleRate       = SR;
    Tx.OutputSampleRate = SR;
    Tx.Reverse          = 0;
    double fc_offset = Tx.Bandwidth * (1.0 - 0.5 / Tx.Tones) / 2.0;
    Tx.FirstCarrierMultiplier = (centerHz - fc_offset) / 500.0;
    if (Tx.Preset() < 0) { fprintf(stderr, "Tx.Preset failed\n"); return {}; }

    std::vector<double> audio;
    std::vector<double> buf(Tx.MaxOutputLen + 8);

    // Lead-in idle so the receiver's synchroniser can lock before real data.
    for (int i = 0; i < 16; i++) Tx.PutChar(0);
    for (char c : text) Tx.PutChar((uint8_t)c);
    for (int i = 0; i < 16; i++) Tx.PutChar(0);   // tail idle to flush FEC pipeline

    Tx.Start();
    bool stopping = false;
    int guard = 0;
    while (Tx.Running() && guard++ < 200000) {
        if (Tx.GetReadReady() < Tx.BitsPerSymbol) {
            if (!stopping) { Tx.Stop(); stopping = true; }
        }
        int len = Tx.Output(buf.data());
        for (int i = 0; i < len; i++) audio.push_back(buf[i]);
    }
    return audio;
}

// Add white gaussian noise for a target SNR measured in the signal bandwidth.
static void addNoise(std::vector<double>& a, double snrDb, unsigned seed) {
    if (a.empty()) return;
    double p = 0; for (double v : a) p += v * v; p /= a.size();
    if (p <= 0) return;
    double npw = p / std::pow(10.0, snrDb / 10.0);
    double sigma = std::sqrt(npw);
    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0.0, sigma);
    for (double& v : a) v += nd(rng);
}

static std::string decode(const std::vector<double>& audio, int tones, int bw,
                          double centerHz, double* snrOut = nullptr) {
    MFSK_Receiver<double> Rx;
    Rx.bContestia      = true;   // Contestia FEC/alphabet
    Rx.Tones           = tones;
    Rx.Bandwidth       = bw;
    Rx.SyncMargin      = 8;
    Rx.SyncIntegLen    = 4;
    Rx.SyncThreshold   = 3.0;
    Rx.SampleRate      = SR;
    Rx.InputSampleRate = SR;
    Rx.Reverse         = 0;
    double fc_offset = Rx.Bandwidth * (1.0 - 0.5 / Rx.Tones) / 2.0;
    Rx.FirstCarrierMultiplier = (centerHz - fc_offset) / 500.0;
    if (Rx.Preset() < 0) { fprintf(stderr, "Rx.Preset failed\n"); return ""; }
    Rx.Reset();

    std::string out;
    const size_t CHUNK = 512;
    std::vector<double> chunk(CHUNK);
    for (size_t off = 0; off < audio.size(); off += CHUNK) {
        size_t n = std::min(CHUNK, audio.size() - off);
        for (size_t i = 0; i < n; i++) chunk[i] = audio[off + i];
        Rx.Process(chunk.data(), n);
        uint8_t ch;
        while (Rx.GetChar(ch) > 0) if (ch >= 7) out.push_back((char)ch);
    }
    Rx.Flush();
    { uint8_t ch; while (Rx.GetChar(ch) > 0) if (ch >= 7) out.push_back((char)ch); }
    if (snrOut) *snrOut = Rx.SignalToNoiseRatio();
    return out;
}

// strip NUL idle chars for display/compare
static std::string clean(const std::string& s) {
    std::string o; for (char c : s) if (c != 0) o.push_back(c); return o;
}

int main(int argc, char** argv) {
    struct Mode { const char* name; int tones; int bw; };
    Mode modes[] = {
        {"Cont4-125", 4, 125},  {"Cont4-250", 4, 250},  {"Cont4-500", 4, 500},
        {"Cont4-1K", 4, 1000},  {"Cont4-2K", 4, 2000},
        {"Cont8-125", 8, 125},  {"Cont8-250", 8, 250},  {"Cont8-500", 8, 500},
        {"Cont8-1K", 8, 1000},  {"Cont8-2K", 8, 2000},
        {"Cont16-250", 16, 250},{"Cont16-500", 16, 500},{"Cont16-1K", 16, 1000},
        {"Cont16-2K", 16, 2000},
        {"Cont32-1K", 32, 1000},{"Cont32-2K", 32, 2000},
        {"Cont64-500", 64, 500},{"Cont64-1K", 64, 1000},{"Cont64-2K", 64, 2000},
    };
    std::string msg = "CQ CQ DE TEST CONTESTIA 73 ";
    double center = 1500.0;

    printf("=== Noise-free round-trip ===\n");
    for (auto& m : modes) {
        auto a = encode(msg, m.tones, m.bw, center);
        double snr = 0;
        std::string got = clean(decode(a, m.tones, m.bw, center, &snr));
        bool ok = got.find("CONTESTIA") != std::string::npos;
        printf("%-11s %6.2fs audio  SNR=%5.1f  %s  \"%s\"\n",
               m.name, a.size() / SR, snr, ok ? "PASS" : "FAIL", got.c_str());
    }

    printf("\n=== Cont8-250 vs noise (target SNR in-band) ===\n");
    for (double snrDb : {30.0, 10.0, 6.0, 3.0, 0.0, -3.0, -6.0}) {
        auto a = encode(msg, 8, 250, center);
        addNoise(a, snrDb, 12345);
        std::string got = clean(decode(a, 8, 250, center));
        bool ok = got.find("CONTESTIA") != std::string::npos;
        printf("SNR %+5.1f dB  %s  \"%s\"\n", snrDb, ok ? "PASS" : "FAIL", got.c_str());
    }
    return 0;
}
