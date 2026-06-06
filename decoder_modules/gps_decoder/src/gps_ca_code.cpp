// gps_ca_code.cpp -- GPS L1 C/A Gold code generator

#include "gps_ca_code.h"

#include <stdexcept>

namespace gps {

// G2 phase selector taps for each PRN (1..32). The values are the two
// 1-indexed cell positions in the G2 shift register whose outputs are XOR'd
// to produce the G2 contribution for that satellite. Taken verbatim from
// IS-GPS-200 Table 3-Ia (Code Phase Assignments).
struct G2Taps { int tap1; int tap2; };

static const G2Taps G2_PHASE_TAPS[NUM_SATELLITES] = {
    {2,  6},   // PRN  1
    {3,  7},   // PRN  2
    {4,  8},   // PRN  3
    {5,  9},   // PRN  4
    {1,  9},   // PRN  5
    {2, 10},   // PRN  6
    {1,  8},   // PRN  7
    {2,  9},   // PRN  8
    {3, 10},   // PRN  9
    {2,  3},   // PRN 10
    {3,  4},   // PRN 11
    {5,  6},   // PRN 12
    {6,  7},   // PRN 13
    {7,  8},   // PRN 14
    {8,  9},   // PRN 15
    {9, 10},   // PRN 16
    {1,  4},   // PRN 17
    {2,  5},   // PRN 18
    {3,  6},   // PRN 19
    {4,  7},   // PRN 20
    {5,  8},   // PRN 21
    {6,  9},   // PRN 22
    {1,  3},   // PRN 23
    {4,  6},   // PRN 24
    {5,  7},   // PRN 25
    {6,  8},   // PRN 26
    {7,  9},   // PRN 27
    {8, 10},   // PRN 28
    {1,  6},   // PRN 29
    {2,  7},   // PRN 30
    {3,  8},   // PRN 31
    {4,  9}    // PRN 32
};

// Shift-register helper: shifts `reg` (10 cells, index 0 = leftmost cell = #1
// in the GPS literature) one step to the right with feedback from `feedback`
// (XOR of the listed 1-indexed cells), returns the bit that fell off the
// right (= output of cell #10) BEFORE the shift.
static inline int shiftLfsr(int reg[10], const int feedbackTaps[], int nTaps) {
    int out = reg[9];                       // output = cell #10
    int fb  = 0;
    for (int i = 0; i < nTaps; i++) {
        fb ^= reg[feedbackTaps[i] - 1];
    }
    for (int i = 9; i > 0; i--) {           // shift right
        reg[i] = reg[i - 1];
    }
    reg[0] = fb;                            // new bit into cell #1
    return out;
}

void generateCaCode(int prn, std::array<int8_t, CA_CODE_LENGTH>& out) {
    if (prn < 1 || prn > NUM_SATELLITES) {
        throw std::out_of_range("PRN must be 1..32");
    }
    const G2Taps& taps = G2_PHASE_TAPS[prn - 1];

    int G1[10], G2[10];
    for (int i = 0; i < 10; i++) { G1[i] = 1; G2[i] = 1; } // both registers = all ones at epoch

    static const int G1_FB[] = { 3, 10 };                       // 1 + x^3 + x^10
    static const int G2_FB[] = { 2, 3, 6, 8, 9, 10 };           // 1 + x^2 + x^3 + x^6 + x^8 + x^9 + x^10

    for (int i = 0; i < CA_CODE_LENGTH; i++) {
        // G2 output = XOR of the two selected phase taps (before shifting)
        int g2out = G2[taps.tap1 - 1] ^ G2[taps.tap2 - 1];
        int g1out = shiftLfsr(G1, G1_FB, 2);
        (void)shiftLfsr(G2, G2_FB, 6);  // we already captured g2out from the pre-shift state

        int chip = g1out ^ g2out;       // 0 or 1
        out[i] = chip ? -1 : +1;        // standard mapping: 0 -> +1, 1 -> -1 for BPSK
    }
}

void generateCaCodeResampled(int prn, int samplesPerMs, std::vector<float>& out) {
    std::array<int8_t, CA_CODE_LENGTH> chips;
    generateCaCode(prn, chips);

    out.resize(samplesPerMs);
    // Nearest-chip mapping. With samplesPerMs >> 1023 the rounding error is
    // sub-chip and unimportant for acquisition; tracking refines fractional
    // alignment separately.
    for (int n = 0; n < samplesPerMs; n++) {
        int chipIdx = (int)((long long)n * CA_CODE_LENGTH / samplesPerMs);
        if (chipIdx >= CA_CODE_LENGTH) chipIdx = CA_CODE_LENGTH - 1;
        out[n] = (float)chips[chipIdx];
    }
}

} // namespace gps
