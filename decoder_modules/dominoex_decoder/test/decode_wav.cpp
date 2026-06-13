// Decode a mono 48 kHz WAV with the THOR modem. Usage:
//   decode_wav <file.wav> <modeIdx> [afStart afEnd afStep]
// Scans AF if a range is given, printing the printable-character yield so the
// real-signal centre frequency can be located.
#include "dominoex/modem.h"
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>
using namespace dominoex;

static std::vector<float> readWav(const char* path, int& sr) {
    FILE* f = fopen(path, "rb"); std::vector<float> out;
    if (!f) { printf("cannot open %s\n", path); return out; }
    uint8_t hdr[44]; fread(hdr, 1, 44, f);
    sr = *(int32_t*)(hdr + 24);
    int16_t bits = *(int16_t*)(hdr + 34);
    int16_t ch = *(int16_t*)(hdr + 22);
    // find data chunk robustly
    fseek(f, 12, SEEK_SET);
    char id[5] = {0}; uint32_t sz = 0; long dataPos = 44; uint32_t dataSz = 0;
    while (fread(id, 1, 4, f) == 4) {
        fread(&sz, 4, 1, f);
        if (memcmp(id, "data", 4) == 0) { dataPos = ftell(f); dataSz = sz; break; }
        fseek(f, sz, SEEK_CUR);
    }
    fseek(f, dataPos, SEEK_SET);
    int n = dataSz / (bits / 8);
    std::vector<int16_t> raw(n); fread(raw.data(), 2, n, f); fclose(f);
    out.reserve(n / ch);
    for (int i = 0; i < n; i += ch) out.push_back(raw[i] / 32768.0f);
    return out;
}

static std::string decode(int modeIdx, double af, const std::vector<float>& a, bool afc = false) {
    DominoEXModem m; m.configure(modeIdx, af); m.setAFC(afc);
    std::string got;
    const int CH = 4096;
    for (size_t off = 0; off < a.size(); off += CH) {
        int n = (int)std::min((size_t)CH, a.size() - off);
        m.process(a.data() + off, n, [&](char c) { got.push_back(c); });
    }
    return got;
}

static int printable(const std::string& s) {
    int p = 0; for (char c : s) if ((c >= 32 && c < 127) || c == '\n') p++;
    return p;
}

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: %s file.wav modeIdx [afStart afEnd afStep]\n", argv[0]); return 1; }
    int sr; std::vector<float> a = readWav(argv[1], sr);
    if (a.empty()) return 1;
    printf("loaded %s : sr=%d samples=%zu dur=%.1fs\n", argv[1], sr, a.size(), a.size() / (double)sr);
    int mode = atoi(argv[2]);

    if (argc >= 6) {
        double a0 = atof(argv[3]), a1 = atof(argv[4]), step = atof(argv[5]);
        double bestAf = a0; int bestP = -1; std::string bestS;
        for (double af = a0; af <= a1; af += step) {
            std::string g = decode(mode, af, a);
            int p = printable(g);
            printf("AF %6.0f Hz : printable=%4d\n", af, p);
            if (p > bestP) { bestP = p; bestAf = af; bestS = g; }
        }
        printf("\nBEST AF %.0f Hz (printable=%d):\n", bestAf, bestP);
        std::string show; for (char c : bestS) if ((c >= 32 && c < 127) || c == '\n') show.push_back(c);
        printf("%s\n", show.c_str());
    } else {
        double af = (argc >= 4) ? atof(argv[3]) : 1500.0;
        bool afc = false;
        for (int i = 1; i < argc; i++) if (std::string(argv[i]) == "afc") afc = true;
        std::string g = decode(mode, af, a, afc);
        std::string show; for (char c : g) if ((c >= 32 && c < 127) || c == '\n') show.push_back(c);
        printf("AF %.0f Hz (AFC %s):\n%s\n", af, afc ? "on" : "off", show.c_str());
    }
    return 0;
}
