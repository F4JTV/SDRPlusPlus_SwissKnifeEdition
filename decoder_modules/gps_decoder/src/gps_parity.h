// gps_parity.h -- GPS L1 C/A navigation-word parity check
//
// Each 30-bit NAV word carries 24 data bits and 6 parity bits computed with
// an extended Hamming (32,26) code, using bits D29* and D30* (the last two
// bits of the previous word) as auxiliary inputs. See IS-GPS-200 table
// 20-XIV for the exact parity equations.
//
// The same equations also explain the "polarity inversion" the bit
// synchroniser sees: when D30* == 1, every data bit d1..d24 of the current
// word is transmitted inverted. This module returns the un-inverted data
// bits when parity passes.

#pragma once

#include <array>
#include <cstdint>

namespace gps {

// Apply IS-GPS-200 parity check to one 30-bit NAV word.
//
//   word      : packed 30-bit word, MSB-first in bits 29..0
//   d29_prev  : bit D29* (bit 29 of the previous word)
//   d30_prev  : bit D30* (bit 30 of the previous word)
//   out_data  : on success, receives 24 un-inverted data bits (MSB-first
//               in bits 23..0). May be nullptr.
//
// Returns true if the 6 received parity bits match the recomputed ones.
bool checkWordParity(uint32_t word, bool d29_prev, bool d30_prev,
                     uint32_t* out_data);

// Convenience: take 30 ±1 soft bits (preamble convention) and pack into a
// uint32_t with bit 29 = first bit of the word.
inline uint32_t packBits(const int8_t* bits) {
    uint32_t w = 0;
    for (int k = 0; k < 30; k++) {
        w <<= 1;
        if (bits[k] > 0) w |= 1u;
    }
    return w;
}

} // namespace gps
