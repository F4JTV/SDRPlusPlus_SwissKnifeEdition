#pragma once
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
//  Convolutional Viterbi decoder, constraint length K = 7, rate R = 1/2.
//
//  FLDIGI / MultiPsk PSK63F uses NON-STANDARD polynomials (psk.cxx ~72-74):
//      FLDIGI POLY1 = 0x6d (octal 155)
//      FLDIGI POLY2 = 0x4f (octal 117)
//
//  This decoder uses a SHIFT-IN-AT-MSB state representation. The
//  polynomials must be bit-reversed (7-bit) to produce the same parities
//  as FLDIGI's encoder (which shifts in at the LSB):
//      G1 = bit_reverse_7(0x6d) = bit_reverse_7(0b1101101) = 0b1011011 = 0x5B
//      G2 = bit_reverse_7(0x4f) = bit_reverse_7(0b1001111) = 0b1111001 = 0x79
//
//  NOTE: these happen to numerically match the well-known NASA Voyager
//  polynomials (171/133 octal = 0x79/0x5B). DO NOT be misled - they are
//  assigned to G1/G2 in the OPPOSITE order. G1=0x5B, G2=0x79.
//
//  Encoded symbols are fed interleaved (G1, G2, G1, G2, ...).
//  Hard-decision implementation. 64 states. TBLEN survivor traceback.
//
//  Output: one decoded bit per pair of input symbols, with ~TBLEN bits of
//  decoding latency.
//
//  State convention: ps holds the K-1 previous input bits, newest bit at
//  position K-2 (bit 5), oldest at bit 0. When new bit `b` arrives:
//      reg = (b << (K-1)) | ps
//      ns  = (ps >> 1) | (b << (K-2))
// ---------------------------------------------------------------------------

namespace fldigi {

    class ViterbiK7R12 {
    public:
        static constexpr int K        = 7;
        static constexpr int N_STATES = 1 << (K - 1);  // 64
        static constexpr int TBLEN    = 64;            // traceback length
        static constexpr int BUFLEN   = TBLEN + 1;     // +1 so read != write slot
        static constexpr int POLY_G1  = 0x5B;          // bit_reverse_7(0x6d)
        static constexpr int POLY_G2  = 0x79;          // bit_reverse_7(0x4f)
        static constexpr int METRIC_INF = 4096;

        ViterbiK7R12() { reset(); }

        void reset() {
            for (int s = 0; s < N_STATES; s++) { pm[s] = (s == 0) ? 0 : METRIC_INF; }
            symIdx   = 0;
            s0       = 0;
            tbHead   = 0;
            tbFilled = 0;
            std::memset(tb, 0, sizeof(tb));
        }

        // Feed one received bit. Returns:
        //   -1 : waiting for 2nd symbol or traceback warming up
        //  0/1 : a decoded bit
        int process(int rxBit) {
            if (symIdx == 0) {
                s0 = rxBit & 1;
                symIdx = 1;
                return -1;
            }
            int s1 = rxBit & 1;
            symIdx = 0;

            // Compute next-state metrics & survivors for this step
            int npm[N_STATES];
            uint8_t prev_for[N_STATES];
            for (int s = 0; s < N_STATES; s++) {
                npm[s]      = METRIC_INF;
                prev_for[s] = 0;
            }

            for (int ps = 0; ps < N_STATES; ps++) {
                int pmps = pm[ps];
                if (pmps >= METRIC_INF) { continue; }
                for (int bit = 0; bit < 2; bit++) {
                    unsigned reg = ((unsigned)bit << (K - 1)) | (unsigned)ps;
                    int c0 = __builtin_popcount(reg & POLY_G1) & 1;
                    int c1 = __builtin_popcount(reg & POLY_G2) & 1;
                    int bm = (c0 ^ s0) + (c1 ^ s1);
                    int m  = pmps + bm;
                    int ns = (ps >> 1) | (bit << (K - 2));
                    if (m < npm[ns]) {
                        npm[ns]      = m;
                        prev_for[ns] = (uint8_t)ps;
                    }
                }
            }

            // Normalize visited metrics (subtract min). Unvisited stay at INF.
            int mn = METRIC_INF;
            for (int s = 0; s < N_STATES; s++) { if (npm[s] < mn) { mn = npm[s]; } }
            for (int s = 0; s < N_STATES; s++) {
                if (npm[s] < METRIC_INF) { pm[s] = npm[s] - mn; }
                else                     { pm[s] = METRIC_INF; }
            }

            // Append step to ring buffer (size BUFLEN = TBLEN+1)
            for (int s = 0; s < N_STATES; s++) {
                tb[tbHead].prev[s] = prev_for[s];
            }
            int writeIdx = tbHead;
            tbHead = (tbHead + 1) % BUFLEN;

            // Wait until we have TBLEN entries
            if (tbFilled < TBLEN) { tbFilled++; return -1; }

            // Best state right now
            int best = 0;
            for (int s = 1; s < N_STATES; s++) { if (pm[s] < pm[best]) { best = s; } }

            // Traceback from newest (writeIdx) back TBLEN-1 steps to reach
            // the oldest step still in the buffer.
            int idx = writeIdx;
            int st  = best;
            for (int i = 0; i < TBLEN; i++) {
                st  = tb[idx].prev[st];
                idx = (idx - 1 + BUFLEN) % BUFLEN;
            }
            // st = state at the oldest step. The bit that produced this state
            // is the newest bit IN st, i.e. bit (K-2).
            return (st >> (K - 2)) & 1;
        }

    private:
        int pm[N_STATES];
        struct TbEntry {
            uint8_t prev[N_STATES];
        } tb[BUFLEN];
        int tbHead;    // next write position
        int tbFilled;  // number of valid entries (caps at TBLEN)
        int symIdx;    // 0 or 1, position in (s0, s1) pair
        int s0;
    };

}
