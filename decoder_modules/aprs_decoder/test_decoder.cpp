// End-to-end test of the APRS decode chain (afsk.h + ax25.h), no SDR++ deps.
// Builds a real AX.25/APRS frame, HDLC+NRZI+stuffs it, renders Bell-202 AFSK
// audio at 24 kHz, then runs it back through AFSK1200 + parseAX25 + parseAPRS.

#include "aprs/afsk.h"
#include "aprs/ax25.h"
#include <vector>
#include <string>
#include <cstdio>
#include <cmath>
#include <cstring>

using namespace aprs;

// --- build a 7-byte AX.25 address ---
static void pushAddr(std::vector<uint8_t>& v, const char* call, int ssid, bool last) {
    char c[6] = {' ',' ',' ',' ',' ',' '};
    int n = (int)strlen(call); if (n > 6) n = 6;
    for (int i = 0; i < n; i++) c[i] = call[i];
    for (int i = 0; i < 6; i++) v.push_back((uint8_t)(c[i] << 1));
    uint8_t ss = (uint8_t)(0x60 | ((ssid & 0x0F) << 1) | (last ? 0x01 : 0x00));
    v.push_back(ss);
}

int main() {
    // 1) Build the AX.25 frame: dest, source, digi, control, pid, info
    std::vector<uint8_t> frame;
    pushAddr(frame, "APRS", 0, false);   // dest
    pushAddr(frame, "F4JTV", 9, false);  // source SSID-9
    pushAddr(frame, "WIDE1", 1, true);   // one digi, last addr
    frame.push_back(0x03);               // control: UI
    frame.push_back(0xF0);               // pid: no layer 3
    // Uncompressed position near Marseille, with course/speed extension.
    std::string info = "=4318.00N/00522.00E>088/036Test ADRASEC 06";
    for (char ch : info) frame.push_back((uint8_t)ch);

    // 2) Append FCS (CRC-16/X.25), low byte first
    uint16_t fcs = crc16_x25(frame.data(), frame.size());
    frame.push_back((uint8_t)(fcs & 0xFF));
    frame.push_back((uint8_t)((fcs >> 8) & 0xFF));

    // 3) Build HDLC bit stream: flags + LSB-first stuffed data + flags
    std::vector<int> bits;
    auto putFlag = [&]() { // 0x7E, palindrome so order irrelevant
        int v = 0x7E;
        for (int i = 0; i < 8; i++) bits.push_back((v >> i) & 1);
    };
    for (int i = 0; i < 20; i++) putFlag(); // preamble flags
    int ones = 0;
    for (uint8_t b : frame) {
        for (int i = 0; i < 8; i++) {        // LSB first
            int bit = (b >> i) & 1;
            bits.push_back(bit);
            if (bit) { if (++ones == 5) { bits.push_back(0); ones = 0; } }
            else ones = 0;
        }
    }
    for (int i = 0; i < 6; i++) putFlag();   // trailing flags

    // 4) NRZI encode: bit 0 = toggle tone, bit 1 = keep tone
    std::vector<int> tones; // 1 = mark(1200), 0 = space(2200)
    int cur = 1;
    for (int b : bits) { if (b == 0) cur ^= 1; tones.push_back(cur); }

    // 5) Render AFSK audio at 24 kHz, 20 samples/bit, continuous phase
    const double fs = AFSK_SAMPLE_RATE;
    const int spb = CORRLEN; // 20
    std::vector<float> audio;
    double phase = 0.0;
    for (int t : tones) {
        double f = (t == 1) ? AFSK_MARK_HZ : AFSK_SPACE_HZ;
        for (int k = 0; k < spb; k++) {
            audio.push_back((float)sin(phase));
            phase += 2.0 * M_PI * f / fs;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
        }
    }
    // a little lead-in/lead-out silence
    std::vector<float> sig(200, 0.0f);
    sig.insert(sig.end(), audio.begin(), audio.end());
    sig.insert(sig.end(), 200, 0.0f);

    printf("Frame bytes: %zu, HDLC bits: %zu, audio samples: %zu\n",
           frame.size(), bits.size(), sig.size());

    // 6) Decode
    int frames = 0;
    AFSK1200 demod;
    APRSRecord got; AX25Frame gotFr; bool ok = false;
    demod.setFrameHandler([&](const uint8_t* f, int len) {
        frames++;
        AX25Frame fr;
        if (parseAX25(f, len, fr)) {
            APRSRecord r = parseAPRS(fr);
            gotFr = fr; got = r; ok = true;
            printf("\n--- DECODED FRAME #%d ---\n", frames);
            printf("  source = %s   dest = %s\n", fr.source.c_str(), fr.dest.c_str());
            printf("  path   = ");
            for (auto& p : fr.path) printf("%s ", p.c_str());
            printf("\n  type   = %s\n", r.typeDesc.c_str());
            printf("  info   = '%s'\n", fr.info.c_str());
            if (r.hasPosition) printf("  POS    = %.5f, %.5f  sym=%c%c\n",
                                      r.lat, r.lon, r.symbolTable, r.symbolCode);
            if (r.hasCourse)   printf("  course = %d deg\n", r.course);
            if (r.hasSpeed)    printf("  speed  = %.1f kn\n", r.speedKnots);
            if (!r.comment.empty()) printf("  comment= '%s'\n", r.comment.c_str());
        }
    });
    demod.process(sig.data(), (int)sig.size());

    // 7) Validate
    printf("\n=== VALIDATION ===\n");
    bool pass = ok
        && gotFr.source == "F4JTV-9"
        && gotFr.dest   == "APRS"
        && got.hasPosition
        && fabs(got.lat - (43.0 + 18.0/60.0)) < 1e-4
        && fabs(got.lon - (5.0 + 22.0/60.0)) < 1e-4
        && got.hasCourse && got.course == 88
        && got.hasSpeed && fabs(got.speedKnots - 36.0) < 0.5;
    printf("Frames decoded: %d\n", frames);
    printf("RESULT: %s\n", pass ? "PASS ✅" : "FAIL ❌");
    return pass ? 0 : 1;
}
