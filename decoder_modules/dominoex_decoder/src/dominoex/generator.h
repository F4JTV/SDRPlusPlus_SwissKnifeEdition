#pragma once
// DominoEX test-signal generator (transmit path), faithful port of fldigi
// src/dominoex/dominoex.cxx TX (non-FEC): varicode -> IFK+ tone -> cosine.
// Produces real audio at AUDIO_SR (48 kHz) so the full decoder chain (including
// the 48k -> internal decimator) is exercised.
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include "modem.h"
#include "varicode.h"

namespace dominoex {

class DominoEXGenerator {
public:
    // modeIdx: index into DOMINOEX_MODES; afFreq: signal centre frequency (Hz).
    DominoEXGenerator(int modeIdx, double afFreq)
        : idx(std::clamp(modeIdx, 0, DOMINOEX_MODE_COUNT - 1)), af(afFreq) {
        const DominoMode& M = DOMINOEX_MODES[idx];
        if (M.nativeSR == 8000) { internalSR = 8000.0;  decim = 6; }
        else                    { internalSR = 12000.0; decim = 4; }
        doublespaced = M.doublespaced;
        symlen = (int)std::lround((double)M.symlen * internalSR / M.nativeSR);
        if (symlen < 8) symlen = 8;
        tonespacing = internalSR * doublespaced / symlen;
        bandwidth = NUMTONES * tonespacing;
        sampPerSym = decim * symlen;   // 48k samples per symbol
    }

    // Generate audio for a message, bracketed by the fldigi preamble / start /
    // end / flush sequence so the receiver's varicode framing fills correctly.
    std::vector<float> generate(const std::string& text) {
        out.clear();
        txprevtone = 0; txphase = 0.0;

        bool micro = (idx == 0);

        // preamble: idle <NUL> (secondary)
        sendidle();
        // start
        sendchar('\r', 0);
        if (!micro) { sendchar(2, 0); sendchar('\r', 0); }   // STX
        // data
        for (unsigned char c : text) sendchar(c, 0);
        // end
        sendchar('\r', 0);
        if (!micro) { sendchar(4, 0); sendchar('\r', 0); }   // EOT
        // flush: idle characters to push the tail through the receiver
        for (int i = 0; i < 4; i++) sendidle();

        return out;
    }

    double sampleRate() const { return AUDIO_SR; }

private:
    void sendidle() { sendchar(0, 1); }   // secondary <NUL>

    void sendchar(unsigned char c, int secondary) {
        const unsigned char* code = dominoex_varienc(c, secondary);
        sendsymbol(code[0]);
        // continuation nibbles all have the MSB set
        for (int sym = 1; sym < 3; sym++) {
            if (code[sym] & 0x8) sendsymbol(code[sym]);
            else break;
        }
    }

    void sendsymbol(int sym) {
        int tone = (txprevtone + 2 + sym) % NUMTONES;
        txprevtone = tone;
        if (reverse) tone = (NUMTONES - 1) - tone;
        sendtone(tone);
    }

    void sendtone(int tone) {
        double f = (tone + 0.5) * tonespacing + af - bandwidth / 2.0;
        double phaseincr = DOMINOEX_TWOPI * f / AUDIO_SR;
        for (int i = 0; i < sampPerSym; i++) {
            out.push_back((float)cos(txphase));
            txphase -= phaseincr;
            if (txphase < 0) txphase += DOMINOEX_TWOPI;
        }
    }

    int idx;
    double af;
    double internalSR; int decim;
    int doublespaced, symlen, sampPerSym;
    double tonespacing, bandwidth;
    bool reverse = false;
    int txprevtone = 0;
    double txphase = 0.0;
    std::vector<float> out;
};

} // namespace dominoex
