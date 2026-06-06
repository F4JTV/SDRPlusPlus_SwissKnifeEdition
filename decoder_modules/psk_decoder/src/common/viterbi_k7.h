#pragma once
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
//  Convolutional Viterbi decoder, constraint length K = 7, rate R = 1/2.
//
//  Standard NASA/Voyager polynomials, also used by MultiPsk / FLDIGI for
//  PSK63F (PSK63FEC):
//      G1 = 171 (octal) = 0x79
//      G2 = 133 (octal) = 0x5B
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
        static constexpr int POLY_G1  = 0x79;          // 171 octal
        static constexpr int POLY_G2  = 0x5B;          // 133 octal
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
