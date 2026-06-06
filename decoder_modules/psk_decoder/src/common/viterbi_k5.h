#pragma once
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
//  Convolutional Viterbi decoder, constraint length K = 5, rate R = 1/2.
//
//  FLDIGI / G3PLX PSK31 QPSK uses polynomials POLY1 = 0x17 (octal 27) and
//  POLY2 = 0x19 (octal 31) in a SHIFT-IN-AT-LSB encoder convention.
//
//  This decoder uses a SHIFT-IN-AT-MSB state representation (ps holds the
//  K-1 previous input bits, newest at bit K-2, oldest at bit 0). For the
//  parity computation to produce the same encoded bits as FLDIGI's encoder,
//  the polynomials must be BIT-REVERSED:
//      G1 = bit_reverse_5(0x17) = bit_reverse_5(0b10111) = 0b11101 = 0x1D
//      G2 = bit_reverse_5(0x19) = bit_reverse_5(0b11001) = 0b10011 = 0x13
//
//  Encoded symbols are fed *interleaved* (G1, G2, G1, G2, ...).
//  Hard-decision implementation. 16 states, TBLEN survivor traceback.
//
//  Output: one decoded bit per pair of input symbols, with ~TBLEN bits of
//  decoding latency.
//
//  State convention: ps holds the K-1 previous input bits, newest bit at
//  position K-2 (bit 3), oldest at bit 0. When new bit b arrives:
//      reg = (b << (K-1)) | ps
//      ns  = (ps >> 1) | (b << (K-2))
// ---------------------------------------------------------------------------

namespace fldigi {

    class ViterbiK5R12 {
    public:
        static constexpr int K        = 5;
        static constexpr int N_STATES = 1 << (K - 1);   // 16
        static constexpr int TBLEN    = 32;             // traceback length
        static constexpr int BUFLEN   = TBLEN + 1;
        // Bit-reversed FLDIGI polynomials so the parity matches the
        // FLDIGI encoder (which shifts new bits in at the LSB).
        static constexpr int POLY_G1  = 0x1D;           // bit_reverse_5(0x17)
        static constexpr int POLY_G2  = 0x13;           // bit_reverse_5(0x19)
        static constexpr int METRIC_INF = 4096;

        ViterbiK5R12() { reset(); }

        void reset() {
            for (int s = 0; s < N_STATES; s++) { pm[s] = (s == 0) ? 0 : METRIC_INF; }
            symIdx   = 0;
            s0       = 0;
            tbHead   = 0;
            tbFilled = 0;
            std::memset(tb, 0, sizeof(tb));
        }

        // Feed one received bit. Returns:
        //   -1 : waiting for 2nd symbol of pair, or traceback still warming up
        //  0/1 : a decoded bit
        int process(int rxBit) {
            if (symIdx == 0) {
                s0 = rxBit & 1;
                symIdx = 1;
                return -1;
            }
            int s1 = rxBit & 1;
            symIdx = 0;

            int     npm[N_STATES];
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

            int mn = METRIC_INF;
            for (int s = 0; s < N_STATES; s++) { if (npm[s] < mn) { mn = npm[s]; } }
            for (int s = 0; s < N_STATES; s++) {
                if (npm[s] < METRIC_INF) { pm[s] = npm[s] - mn; }
                else                     { pm[s] = METRIC_INF; }
            }

            for (int s = 0; s < N_STATES; s++) {
                tb[tbHead].prev[s] = prev_for[s];
            }
            int writeIdx = tbHead;
            tbHead = (tbHead + 1) % BUFLEN;

            if (tbFilled < TBLEN) { tbFilled++; return -1; }

            int best = 0;
            for (int s = 1; s < N_STATES; s++) { if (pm[s] < pm[best]) { best = s; } }

            int idx = writeIdx;
            int st  = best;
            for (int i = 0; i < TBLEN; i++) {
                st  = tb[idx].prev[st];
                idx = (idx - 1 + BUFLEN) % BUFLEN;
            }
            return (st >> (K - 2)) & 1;
        }

    private:
        int pm[N_STATES];
        struct TbEntry {
            uint8_t prev[N_STATES];
        } tb[BUFLEN];
        int tbHead;
        int tbFilled;
        int symIdx;
        int s0;
    };

}
