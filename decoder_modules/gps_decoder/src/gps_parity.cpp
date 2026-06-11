// gps_parity.cpp -- IS-GPS-200 table 20-XIV parity equations.

#include "gps_parity.h"

namespace gps {

// Helper: extract a single bit (1-indexed from MSB; matches the IS-GPS-200
// notation where d1 is the FIRST bit of the word).
static inline int bit1(uint32_t w, int k_1based_msb) {
    // word bit 30 = MSB (b29 in uint32 terms). d_k corresponds to bit
    // 30 - k in uint32 terms, i.e., shift right by (30 - k).
    return (int)((w >> (30 - k_1based_msb)) & 1u);
}

bool checkWordParity(uint32_t word, bool d29_prev, bool d30_prev,
                     uint32_t* out_data)
{
    // Extract the 30 received bits AS-SENT (no pre-inversion). The parity
    // equations are computed on what was actually transmitted; the D30*
    // inversion is applied at the transmitter BEFORE parity, so on the
    // wire we already see the inverted bits.
    int d[31];               // 1-indexed, d[1]..d[30]
    for (int k = 1; k <= 30; k++) d[k] = bit1(word, k);

    int D29s = d29_prev ? 1 : 0;
    int D30s = d30_prev ? 1 : 0;

    auto X = [](std::initializer_list<int> xs) {
        int v = 0; for (int x : xs) v ^= x; return v;
    };

    int D25 = X({ D29s,
                  d[ 1], d[ 2], d[ 3], d[ 5], d[ 6],
                  d[10], d[11], d[12], d[13], d[14],
                  d[17], d[18], d[20], d[23] });
    int D26 = X({ D30s,
                  d[ 2], d[ 3], d[ 4], d[ 6], d[ 7],
                  d[11], d[12], d[13], d[14], d[15],
                  d[18], d[19], d[21], d[24] });
    int D27 = X({ D29s,
                  d[ 1], d[ 3], d[ 4], d[ 5], d[ 7], d[ 8],
                  d[12], d[13], d[14], d[15], d[16],
                  d[19], d[20], d[22] });
    int D28 = X({ D30s,
                  d[ 2], d[ 4], d[ 5], d[ 6], d[ 8], d[ 9],
                  d[13], d[14], d[15], d[16], d[17],
                  d[20], d[21], d[23] });
    int D29 = X({ D30s,
                  d[ 1], d[ 3], d[ 5], d[ 6], d[ 7], d[ 9], d[10],
                  d[14], d[15], d[16], d[17], d[18],
                  d[21], d[22], d[24] });
    int D30 = X({ D29s,
                  d[ 3], d[ 5], d[ 6], d[ 8], d[ 9], d[10], d[11],
                  d[13], d[15], d[19], d[22], d[23], d[24] });

    // Received parity bits = d[25]..d[30].
    if (D25 != d[25]) return false;
    if (D26 != d[26]) return false;
    if (D27 != d[27]) return false;
    if (D28 != d[28]) return false;
    if (D29 != d[29]) return false;
    if (D30 != d[30]) return false;

    if (out_data) {
        // Now un-invert d[1..24] if D30* was set, to recover the original
        // information bits the spec calls "d1..d24".
        uint32_t v = 0;
        for (int k = 1; k <= 24; k++) {
            int b = d[k] ^ (d30_prev ? 1 : 0);
            v = (v << 1) | (uint32_t)b;
        }
        *out_data = v; // 24 bits, MSB = d1 (un-inverted)
    }
    return true;
}

} // namespace gps
