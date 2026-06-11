// gps_ephemeris.cpp -- subframe parsing per IS-GPS-200

#include "gps_ephemeris.h"
#include "gps_parity.h"

#include <cmath>
#include <cstdint>

namespace gps {

namespace {

constexpr double PI = 3.14159265358979323846;

// Pull `length` bits from a 24-bit word, where `start1` is the 1-based MSB
// position in IS-GPS-200 notation (bit 1 = data MSB, bit 24 = data LSB).
inline uint32_t extr(uint32_t w24, int start1, int length) {
    int shift = 24 - start1 - length + 1;
    return (w24 >> shift) & ((1u << length) - 1);
}

// Two's complement sign extension on `length`-bit value `v`.
inline int32_t signExt(uint32_t v, int length) {
    uint32_t sign_mask = 1u << (length - 1);
    if (v & sign_mask) {
        return (int32_t)(v | (~0u << length));
    }
    return (int32_t)v;
}

// Concatenate MSBs from one word and LSBs from another into a value of
// `total` bits.
inline uint32_t cat(uint32_t msb, int msb_len, uint32_t lsb, int lsb_len) {
    (void)msb_len;
    return (msb << lsb_len) | lsb;
}

// 2^n with n possibly negative, as a double.
inline double p2(int n) { return std::ldexp(1.0, n); }

// Pack 30 nav bits into a uint32, MSB-first.
inline uint32_t packWord30(const int8_t* p) { return packBits(p); }

void parseSubframe1(const SubframeData& sf, Ephemeris& eph) {
    // Word 3 (data): WN(10) | code_L2(2) | URA(4) | health(6) | IODC_msb(2)
    uint32_t w3 = sf.words[2];
    eph.week       = (int)extr(w3, 1, 10);
    eph.code_on_L2 = (int)extr(w3, 11, 2);
    eph.ura_index  = (int)extr(w3, 13, 4);
    eph.sv_health  = (int)extr(w3, 17, 6);
    uint32_t iodc_msb = extr(w3, 23, 2);

    // Word 7 (data): reserved(16) | TGD(8)
    uint32_t w7 = sf.words[6];
    int32_t TGD_raw = signExt(extr(w7, 17, 8), 8);
    eph.T_GD = (double)TGD_raw * p2(-31);

    // Word 8 (data): IODC_lsb(8) | toc(16)
    uint32_t w8 = sf.words[7];
    uint32_t iodc_lsb = extr(w8, 1, 8);
    eph.iodc = (int)((iodc_msb << 8) | iodc_lsb);
    uint32_t toc_raw = extr(w8, 9, 16);
    eph.t_oc = (double)toc_raw * p2(4);

    // Word 9 (data): af2(8) | af1(16)
    uint32_t w9 = sf.words[8];
    int32_t af2_raw = signExt(extr(w9, 1, 8), 8);
    int32_t af1_raw = signExt(extr(w9, 9, 16), 16);
    eph.a_f2 = (double)af2_raw * p2(-55);
    eph.a_f1 = (double)af1_raw * p2(-43);

    // Word 10 (data): af0(22) | reserved(2)
    uint32_t w10 = sf.words[9];
    int32_t af0_raw = signExt(extr(w10, 1, 22), 22);
    eph.a_f0 = (double)af0_raw * p2(-31);

    eph.sf1_received = true;
}

void parseSubframe2(const SubframeData& sf, Ephemeris& eph) {
    // Word 3: IODE(8) | Crs(16)
    uint32_t w3 = sf.words[2];
    eph.iode_sf2 = (int)extr(w3, 1, 8);
    int32_t Crs_raw = signExt(extr(w3, 9, 16), 16);
    eph.C_rs = (double)Crs_raw * p2(-5);

    // Word 4: dn(16) | M0_msb(8)
    uint32_t w4 = sf.words[3];
    int32_t dn_raw = signExt(extr(w4, 1, 16), 16);
    eph.dn = (double)dn_raw * p2(-43) * PI; // semicircles/s -> rad/s
    uint32_t M0_msb = extr(w4, 17, 8);

    // Word 5: M0_lsb(24)  -> total 32 bits signed, scale 2^-31 semicircles
    uint32_t w5 = sf.words[4];
    uint32_t M0_lsb = extr(w5, 1, 24);
    uint32_t M0_raw = (M0_msb << 24) | M0_lsb;
    int32_t M0_s = (int32_t)M0_raw; // already 32-bit signed
    eph.M0 = (double)M0_s * p2(-31) * PI;

    // Word 6: Cuc(16) | e_msb(8)
    uint32_t w6 = sf.words[5];
    int32_t Cuc_raw = signExt(extr(w6, 1, 16), 16);
    eph.C_uc = (double)Cuc_raw * p2(-29);
    uint32_t e_msb = extr(w6, 17, 8);

    // Word 7: e_lsb(24)  -> total 32 bits UNSIGNED, scale 2^-33
    uint32_t w7 = sf.words[6];
    uint32_t e_lsb = extr(w7, 1, 24);
    uint32_t e_raw = (e_msb << 24) | e_lsb;
    eph.e = (double)e_raw * p2(-33);

    // Word 8: Cus(16) | sqrtA_msb(8)
    uint32_t w8 = sf.words[7];
    int32_t Cus_raw = signExt(extr(w8, 1, 16), 16);
    eph.C_us = (double)Cus_raw * p2(-29);
    uint32_t sA_msb = extr(w8, 17, 8);

    // Word 9: sqrtA_lsb(24)  -> total 32 bits UNSIGNED, scale 2^-19 m^0.5
    uint32_t w9 = sf.words[8];
    uint32_t sA_lsb = extr(w9, 1, 24);
    uint32_t sA_raw = (sA_msb << 24) | sA_lsb;
    eph.sqrtA = (double)sA_raw * p2(-19);

    // Word 10: toe(16) | fit_flag(1) | AODO(5) | reserved(2)
    uint32_t w10 = sf.words[9];
    uint32_t toe_raw = extr(w10, 1, 16);
    eph.t_oe     = (double)toe_raw * p2(4);
    eph.fit_flag = (int)extr(w10, 17, 1);
    eph.aodo     = (int)extr(w10, 18, 5);

    eph.sf2_received = true;
}

void parseSubframe3(const SubframeData& sf, Ephemeris& eph) {
    // Word 3: Cic(16) | Omega0_msb(8)
    uint32_t w3 = sf.words[2];
    int32_t Cic_raw = signExt(extr(w3, 1, 16), 16);
    eph.C_ic = (double)Cic_raw * p2(-29);
    uint32_t O0_msb = extr(w3, 17, 8);

    // Word 4: Omega0_lsb(24) -> total 32 bits signed, scale 2^-31 semicircles
    uint32_t w4 = sf.words[3];
    uint32_t O0_lsb = extr(w4, 1, 24);
    uint32_t O0_raw = (O0_msb << 24) | O0_lsb;
    int32_t O0_s = (int32_t)O0_raw;
    eph.Omega0 = (double)O0_s * p2(-31) * PI;

    // Word 5: Cis(16) | i0_msb(8)
    uint32_t w5 = sf.words[4];
    int32_t Cis_raw = signExt(extr(w5, 1, 16), 16);
    eph.C_is = (double)Cis_raw * p2(-29);
    uint32_t i0_msb = extr(w5, 17, 8);

    // Word 6: i0_lsb(24) -> total 32 bits signed, scale 2^-31 semicircles
    uint32_t w6 = sf.words[5];
    uint32_t i0_lsb = extr(w6, 1, 24);
    uint32_t i0_raw = (i0_msb << 24) | i0_lsb;
    int32_t i0_s = (int32_t)i0_raw;
    eph.i0 = (double)i0_s * p2(-31) * PI;

    // Word 7: Crc(16) | omega_msb(8)
    uint32_t w7 = sf.words[6];
    int32_t Crc_raw = signExt(extr(w7, 1, 16), 16);
    eph.C_rc = (double)Crc_raw * p2(-5);
    uint32_t w_msb = extr(w7, 17, 8);

    // Word 8: omega_lsb(24) -> 32 bits signed, scale 2^-31 semicircles
    uint32_t w8 = sf.words[7];
    uint32_t w_lsb = extr(w8, 1, 24);
    uint32_t w_raw = (w_msb << 24) | w_lsb;
    int32_t w_s = (int32_t)w_raw;
    eph.omega = (double)w_s * p2(-31) * PI;

    // Word 9: Omega_dot(24)  signed, scale 2^-43 semicircles/s
    uint32_t w9 = sf.words[8];
    int32_t Odot_raw = signExt(extr(w9, 1, 24), 24);
    eph.Omega_dot = (double)Odot_raw * p2(-43) * PI;

    // Word 10: IODE(8) | i_dot(14) | reserved(2)
    uint32_t w10 = sf.words[9];
    eph.iode_sf3 = (int)extr(w10, 1, 8);
    int32_t idot_raw = signExt(extr(w10, 9, 14), 14);
    eph.i_dot = (double)idot_raw * p2(-43) * PI;

    eph.sf3_received = true;
}

} // namespace

bool decodeSubframeWords(const int8_t* bits, int prn,
                         bool d29_prev_w2, bool d30_prev_w2,
                         SubframeData& out)
{
    // Pack the 10 words from the 300-bit stream.
    uint32_t raw[10];
    for (int k = 0; k < 10; k++) {
        raw[k] = packWord30(bits + k * 30);
    }

    // We don't verify TLM (word 1) or HOW (word 2) parity here -- the HOW
    // was already accepted at preamble-detection time. We DO verify words
    // 3..10. For word 3, D29*/D30* come from word 2's bits 29 and 30.
    bool d29p = ((raw[1] >> 1) & 1u) != 0; // bit 29 of word 2 is bit index 1 in uint32
    bool d30p = (raw[1] & 1u) != 0;        // bit 30 = LSB

    // Override if caller supplied (the very first decode bootstraps).
    (void)d29_prev_w2; (void)d30_prev_w2;

    out.prn = prn;
    out.subframeId = 0;

    // Subframe ID lives in bits 20..22 of word 2 (HOW). Extract from the
    // already-packed raw[1] for SubframeData population.
    out.subframeId = (int)((raw[1] >> 8) & 0x7u);

    for (int k = 0; k < 10; k++) out.words[k] = 0;
    // Copy words 1 and 2 raw, low-24 bits (we'll skip parity here).
    out.words[0] = (raw[0] >> 6) & 0xFFFFFFu;
    out.words[1] = (raw[1] >> 6) & 0xFFFFFFu;

    for (int k = 2; k < 10; k++) {
        uint32_t data24 = 0;
        if (!checkWordParity(raw[k], d29p, d30p, &data24)) {
            return false;
        }
        out.words[k] = data24;
        // For the next word, use this word's bits 29 and 30.
        d29p = ((raw[k] >> 1) & 1u) != 0;
        d30p = (raw[k] & 1u) != 0;
    }
    return true;
}

bool applySubframeToEphemeris(const SubframeData& sf, Ephemeris& eph) {
    if (eph.prn == 0) eph.prn = sf.prn;
    if (eph.prn != sf.prn) return false;
    switch (sf.subframeId) {
        case 1: parseSubframe1(sf, eph); return true;
        case 2: parseSubframe2(sf, eph); return true;
        case 3: parseSubframe3(sf, eph); return true;
        default: return false;
    }
}

} // namespace gps
