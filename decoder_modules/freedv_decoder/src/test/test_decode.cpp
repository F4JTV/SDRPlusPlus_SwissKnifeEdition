/*
 * test_decode.cpp - standalone decode harness.
 *
 * Reads an 8 kHz / 16-bit mono WAV (e.g. produced by gen_freedv), normalises it
 * to +/-1.0 floats, and feeds it to fdvmod::FreeDVCore exactly as the SDR++
 * module does. Reports sync rate, mean SNR estimate, decoded speech energy and
 * the recovered text-channel string, so we can confirm the decoder works and
 * degrades gracefully as the channel SNR drops.
 *
 * Build: see the test/ section of the module README.
 */
#include "../freedv_core.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

// Minimal WAV reader for PCM16 mono. Returns sample rate, fills samples.
static int read_wav(const char* path, std::vector<short>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    char tag[4]; uint32_t u32; uint16_t u16;
    fread(tag, 1, 4, f);            // RIFF
    fread(&u32, 4, 1, f);           // size
    fread(tag, 1, 4, f);            // WAVE
    int fs = 8000, dataBytes = 0; long dataPos = 0;
    while (fread(tag, 1, 4, f) == 4) {
        uint32_t chunk; fread(&chunk, 4, 1, f);
        if (!memcmp(tag, "fmt ", 4)) {
            long here = ftell(f);
            fread(&u16, 2, 1, f);    // fmt
            fread(&u16, 2, 1, f);    // channels
            fread(&u32, 4, 1, f);    // sample rate
            fs = (int)u32;
            fseek(f, here + chunk, SEEK_SET);
        } else if (!memcmp(tag, "data", 4)) {
            dataBytes = (int)chunk; dataPos = ftell(f);
            fseek(f, chunk, SEEK_CUR);
        } else {
            fseek(f, chunk, SEEK_CUR);
        }
    }
    out.resize(dataBytes / 2);
    fseek(f, dataPos, SEEK_SET);
    fread(out.data(), 2, out.size(), f);
    fclose(f);
    return fs;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <mode> <in.wav> [expect_text]\n"
                        "  mode: 1600 700C 700D 700E 800XA 2020 2020B\n", argv[0]);
        return 1;
    }
    const char* modeName = argv[1];
    int mode = -1;
    int nm = 0; const fdvmod::ModeInfo* mt = fdvmod::modeTable(&nm);
    for (int i = 0; i < nm; i++) { if (!strcmp(mt[i].name, modeName)) { mode = mt[i].id; } }
    if (mode < 0) { fprintf(stderr, "unknown mode %s\n", modeName); return 1; }

    std::vector<short> wav;
    int fs = read_wav(argv[2], wav);

    fdvmod::FreeDVCore core;
    if (!core.open(mode)) { fprintf(stderr, "FreeDVCore.open(%s) failed\n", modeName); return 1; }
    if (fs != core.getModemSampleRate()) {
        fprintf(stderr, "warning: wav fs=%d but mode modem fs=%d\n", fs, core.getModemSampleRate());
    }

    // Feed the signal in small blocks (like the SDR++ stream would), and
    // accumulate decode statistics.
    std::vector<float> block, speech;
    int syncBlocks = 0, totalBlocks = 0;
    double snrSum = 0.0; int snrN = 0;
    double speechEnergy = 0.0;
    size_t i = 0;
    const int BLK = 2048;
    while (i < wav.size()) {
        int n = (int)std::min((size_t)BLK, wav.size() - i);
        block.resize(n);
        for (int k = 0; k < n; k++) { block[k] = wav[i + k] / 32768.0f; }
        i += n;

        speech.clear();
        core.process(block.data(), n, speech);

        totalBlocks++;
        if (core.synced()) {
            syncBlocks++;
            snrSum += core.snr(); snrN++;
        }
        for (float s : speech) { speechEnergy += (double)s * s; }
    }

    std::string text = core.getText();
    double syncRate = totalBlocks ? 100.0 * syncBlocks / totalBlocks : 0.0;
    double meanSnr = snrN ? snrSum / snrN : 0.0;

    printf("mode=%-5s  sync=%5.1f%%  meanSNRest=%6.2f dB  speechEnergy=%.3e  text=\"%s\"\n",
           modeName, syncRate, meanSnr, speechEnergy, text.c_str());

    bool pass = (syncRate > 50.0) && (speechEnergy > 0.0);
    if (argc >= 4) {
        // Check the expected text token appears in the recovered text channel.
        bool found = text.find(argv[3]) != std::string::npos;
        printf("  text-channel expect \"%s\": %s\n", argv[3], found ? "FOUND" : "not found");
        pass = pass && found;
    }
    printf("  RESULT: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
