// gps_ca_code.h -- GPS L1 C/A Gold code generator
//
// Each visible satellite continuously broadcasts a 1023-chip pseudo-random
// noise (PRN) sequence at 1.023 Mcps. The sequences are Gold codes built
// from two 10-stage maximum-length LFSRs, G1 and G2:
//
//     G1(x) = 1 + x^3  + x^10
//     G2(x) = 1 + x^2  + x^3  + x^6  + x^8  + x^9  + x^10
//
// Each satellite (PRN number) is distinguished by a unique pair of G2
// register taps whose outputs are XOR'd to form the G2 contribution. The
// final chip is (G1_out XOR G2_tapped_out), mapped to bipolar +1 / -1.
//
// Reference: IS-GPS-200 Section 3.3.2 "C/A Code Generator".
//
// The code period is exactly 1 ms (1023 chips at 1.023 MHz).

#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace gps {

constexpr int   CA_CODE_LENGTH = 1023;          // chips per code period
constexpr float CA_CHIP_RATE   = 1023000.0f;    // chips / second
constexpr float CA_CODE_PERIOD = 0.001f;        // seconds (1 ms)
constexpr int   NUM_SATELLITES = 32;            // operational PRN range we care about

// Generate the full 1023-chip C/A code for the given satellite (PRN 1..32).
// Output: bipolar +1 / -1 values, one per chip.
void generateCaCode(int prn, std::array<int8_t, CA_CODE_LENGTH>& out);

// Generate the C/A code resampled to the given number of samples per ms
// (single 1 ms period). Output is +1.0f / -1.0f.
//
// Nearest-neighbour resampling is used because the acquisition correlator
// only needs phase information; the loss from chip-aligned mapping is
// negligible for our parallel code-phase search.
void generateCaCodeResampled(int prn, int samplesPerMs, std::vector<float>& out);

} // namespace gps
