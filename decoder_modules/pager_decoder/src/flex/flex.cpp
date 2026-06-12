// FLEX paging protocol decoder.
//
// The state machine, symbol detector and message parsers are adapted from
// multimon-ng's demod_flex.c (GPL-3.0):
//
//   Copyright 2004,2006,2010 Free Software Foundation, Inc.
//   Copyright (C) 2015 Craig Shelley
//   Copyright (C) 2024 Jason Lingohr
//   Contributions by Ramon Smits, Bruce Quinton, Rob0101, bertinholland,
//   bierviltje and others.
//
// BCH(31,21) error correction comes from multimon-ng's bch.c, released
// into the public domain (Unlicense).
//
// Adaptations for SDR++ integration:
//   - Encapsulated the C state struct in a PIMPL inside flex::Decoder
//   - Replaced verbprintf with flog::debug (or no-op for high verbosity)
//   - Replaced stdout output with onMessage(const Message&) callbacks
//   - Removed cJSON output, FIW time-of-day estimation
//   - Group-message capcode propagation is suppressed (the originating
//     short-instruction page still surfaces and so do the resulting
//     alphanumeric pages; the implicit broadcast list is not expanded)

#include "flex.h"
#include "bch.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <ctime>
#include <utils/flog.h>

namespace flex {

// ---------------------------------------------------------------------
// Constants from multimon-ng demod_flex.c
// ---------------------------------------------------------------------
static constexpr uint32_t FLEX_SYNC_MARKER    = 0xA6C6AAAAul;
static constexpr double   SLICE_THRESHOLD     = 0.667;
static constexpr double   DC_OFFSET_FILTER    = 0.010;
static constexpr double   PHASE_LOCKED_RATE   = 0.045;
static constexpr double   PHASE_UNLOCKED_RATE = 0.050;
static constexpr int      LOCK_LEN            = 24;
static constexpr int      IDLE_THRESHOLD      = 0;
static constexpr int      DEMOD_TIMEOUT       = 100;

enum PageTypeInternal {
    FLEX_PAGETYPE_SECURE             = 0,
    FLEX_PAGETYPE_SHORT_INSTRUCTION  = 1,
    FLEX_PAGETYPE_TONE               = 2,
    FLEX_PAGETYPE_STANDARD_NUMERIC   = 3,
    FLEX_PAGETYPE_SPECIAL_NUMERIC    = 4,
    FLEX_PAGETYPE_ALPHANUMERIC       = 5,
    FLEX_PAGETYPE_BINARY             = 6,
    FLEX_PAGETYPE_NUMBERED_NUMERIC   = 7
};

enum StateEnum {
    FLEX_STATE_SYNC1,
    FLEX_STATE_FIW,
    FLEX_STATE_SYNC2,
    FLEX_STATE_DATA
};

// ---------------------------------------------------------------------
// Internal state struct - a near-verbatim copy of the multimon-ng one,
// with fields reordered slightly to match how we access them.
// ---------------------------------------------------------------------
struct Decoder::Impl {
    // ---- Demodulator
    unsigned int sample_freq    = 0;
    double       sample_last    = 0.0;
    int          locked         = 0;
    int          phase          = 0;
    unsigned int sample_count   = 0;
    unsigned int symbol_count   = 0;
    double       envelope_sum   = 0.0;
    int          envelope_count = 0;
    uint64_t     lock_buf       = 0;
    int          symcount[4]    = {0,0,0,0};
    int          timeout        = 0;
    int          nonconsec      = 0;
    unsigned int demod_baud     = 1600;

    // ---- Modulation
    double       symbol_rate    = 0.0;
    double       envelope       = 0.0;
    double       zero           = 0.0;

    // ---- State
    unsigned int sync2_count    = 0;
    unsigned int data_count     = 0;
    unsigned int fiwcount       = 0;
    StateEnum    state_current  = FLEX_STATE_SYNC1;
    StateEnum    state_previous = FLEX_STATE_SYNC1;

    // ---- Sync
    unsigned int sync_code      = 0;
    unsigned int sync_baud      = 0;
    unsigned int sync_levels    = 0;
    unsigned int sync_polarity  = 0;
    uint64_t     syncbuf        = 0;

    // ---- FIW
    unsigned int fiw_rawdata    = 0;
    unsigned int fiw_checksum   = 0;
    unsigned int fiw_cycleno    = 0;
    unsigned int fiw_frameno    = 0;
    unsigned int fiw_fix3       = 0;

    // ---- Data buffers (4 phases x 88 codewords each)
    uint32_t     PhaseA[88]     = {0};
    uint32_t     PhaseB[88]     = {0};
    uint32_t     PhaseC[88]     = {0};
    uint32_t     PhaseD[88]     = {0};
    int          PhaseA_idle    = 0;
    int          PhaseB_idle    = 0;
    int          PhaseC_idle    = 0;
    int          PhaseD_idle    = 0;
    int          phase_toggle   = 0;
    unsigned int data_bit_counter = 0;

    // ---- Decode
    int          long_address   = 0;
    int64_t      capcode        = 0;
    int          page_type      = 0;
};

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------
static inline unsigned int count_bits(unsigned int data) {
#ifdef __GNUC__
    return __builtin_popcount(data);
#else
    unsigned int n = (data >> 1) & 0x77777777;
    data = data - n;
    n = (n >> 1) & 0x77777777;
    data = data - n;
    n = (n >> 1) & 0x77777777;
    data = data - n;
    data = (data + (data >> 4)) & 0x0f0f0f0f;
    data = data * 0x01010101;
    return data >> 24;
#endif
}

static int bch3121_fix_errors(uint32_t* data_to_fix) {
    unsigned int original = *data_to_fix & 0x7FFFFFFF;
    unsigned int data = original;
    int result = bch_flex_correct(&data);
    if (result >= 0) {
        *data_to_fix = data;
        return 0;
    }
    return 1;
}

static unsigned int flex_sync_check(uint64_t buf) {
    unsigned int marker      =  (buf & 0x0000FFFFFFFF0000ULL) >> 16;
    unsigned short codehigh  =  (buf & 0xFFFF000000000000ULL) >> 48;
    unsigned short codelow   = ~(buf & 0x000000000000FFFFULL);
    unsigned int md = count_bits(marker ^ FLEX_SYNC_MARKER);
    unsigned int cd = count_bits((unsigned int)(codelow ^ codehigh));
#ifdef FLEX_TRACE
    fprintf(stderr, "sync probe buf=0x%016llx marker_d=%u code_d=%u ch=0x%x cl=0x%x\n",
            (unsigned long long)buf, md, cd, codehigh, codelow);
#endif
    if (md < 4 && cd < 4) {
        return codehigh;
    }
    return 0;
}

static unsigned int flex_sync(Decoder::Impl* f, unsigned char sym) {
    int retval = 0;
    f->syncbuf = (f->syncbuf << 1) | ((sym < 2) ? 1 : 0);
    retval = flex_sync_check(f->syncbuf);
    if (retval != 0) {
        f->sync_polarity = 0;
    } else {
        retval = flex_sync_check(~f->syncbuf);
        if (retval != 0) { f->sync_polarity = 1; }
    }
    return retval;
}

static void decode_mode(Decoder::Impl* f, unsigned int sync_code) {
    struct ModeEntry { int sync; unsigned int baud; unsigned int levels; };
    static const ModeEntry flex_modes[] = {
        { 0x870C, 1600, 2 },
        { 0xB068, 1600, 4 },
        { 0x7B18, 3200, 2 },
        { 0xDEA0, 3200, 4 },
        { 0x4C7C, 3200, 4 },
        { 0,      0,    0 }
    };
    for (int i = 0; flex_modes[i].sync != 0; i++) {
        if (count_bits(flex_modes[i].sync ^ sync_code) < 4) {
            f->sync_code   = sync_code;
            f->sync_baud   = flex_modes[i].baud;
            f->sync_levels = flex_modes[i].levels;
            return;
        }
    }
}

static inline void read_2fsk(unsigned int sym, unsigned int* dat) {
    *dat = (*dat >> 1) | ((sym > 1) ? 0x80000000u : 0u);
}

static int decode_fiw(Decoder::Impl* f) {
    unsigned int fiw = f->fiw_rawdata;
    if (bch3121_fix_errors(&fiw) != 0) {
        flog::debug("FLEX: Unable to decode FIW");
        return 1;
    }
    f->fiw_checksum = fiw & 0xF;
    f->fiw_cycleno  = (fiw >> 4) & 0xF;
    f->fiw_frameno  = (fiw >> 8) & 0x7F;
    f->fiw_fix3     = (fiw >> 15) & 0x3F;

    unsigned int checksum = (fiw & 0xF);
    checksum += ((fiw >> 4)  & 0xF);
    checksum += ((fiw >> 8)  & 0xF);
    checksum += ((fiw >> 12) & 0xF);
    checksum += ((fiw >> 16) & 0xF);
    checksum += ((fiw >> 20) & 0x01);
    checksum &= 0xF;

    if (checksum != 0xF) {
        flog::debug("FLEX: Bad FIW checksum 0x{:x}", checksum);
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------
// Page-type helpers
// ---------------------------------------------------------------------
static inline bool is_alpha(int t) {
    return t == FLEX_PAGETYPE_ALPHANUMERIC || t == FLEX_PAGETYPE_SECURE;
}
static inline bool is_numeric(int t) {
    return t == FLEX_PAGETYPE_STANDARD_NUMERIC ||
           t == FLEX_PAGETYPE_SPECIAL_NUMERIC  ||
           t == FLEX_PAGETYPE_NUMBERED_NUMERIC;
}
static inline bool is_tone(int t) { return t == FLEX_PAGETYPE_TONE; }

// Map internal type to public enum
static PageType mapType(int t) {
    switch (t) {
        case FLEX_PAGETYPE_SECURE:            return PAGE_TYPE_SECURE;
        case FLEX_PAGETYPE_SHORT_INSTRUCTION: return PAGE_TYPE_SHORT_INSTRUCTION;
        case FLEX_PAGETYPE_TONE:              return PAGE_TYPE_TONE;
        case FLEX_PAGETYPE_STANDARD_NUMERIC:  return PAGE_TYPE_STANDARD_NUMERIC;
        case FLEX_PAGETYPE_SPECIAL_NUMERIC:   return PAGE_TYPE_SPECIAL_NUMERIC;
        case FLEX_PAGETYPE_ALPHANUMERIC:      return PAGE_TYPE_ALPHANUMERIC;
        case FLEX_PAGETYPE_BINARY:            return PAGE_TYPE_BINARY;
        case FLEX_PAGETYPE_NUMBERED_NUMERIC:  return PAGE_TYPE_NUMBERED_NUMERIC;
        default:                              return PAGE_TYPE_UNKNOWN;
    }
}

// ---------------------------------------------------------------------
// Message emission helper - factors out the common Message-build logic
// ---------------------------------------------------------------------
static void emit(Decoder* self, Decoder::Impl* f, char phaseLetter,
                 const std::string& content,
                 bool fragmented, bool groupMessage)
{
    Message m;
    m.timestamp    = std::time(nullptr);
    m.capcode      = f->capcode;
    m.type         = mapType(f->page_type);
    m.content      = content;
    m.phase        = phaseLetter;
    m.baud         = (int)f->sync_baud;
    m.levels       = (int)f->sync_levels;
    m.cycle        = (int)f->fiw_cycleno;
    m.frame        = (int)f->fiw_frameno;
    m.fragmented   = fragmented;
    m.groupMessage = groupMessage;
    self->onMessage(m);
}

// ---------------------------------------------------------------------
// Page parsers - simplified from multimon-ng (no group-list expansion,
// no fragment continuation tracking across messages, no JSON output)
// ---------------------------------------------------------------------
static void parse_alphanumeric(Decoder* self, Decoder::Impl* f,
                               const uint32_t* phaseptr, char phaseNo,
                               int mw1, int mw2, bool groupMessage)
{
    char message[2048];
    int currentChar = 0;
    int frag = (phaseptr[mw1] >> 11) & 0x03;
    int cont = (phaseptr[mw1] >> 0x0A) & 0x01;
    bool fragmented = (cont != 0) || (frag != 3);
    mw1++;

    for (int i = mw1; i <= mw2 && currentChar < (int)sizeof(message) - 1; i++) {
        unsigned int dw = phaseptr[i];
        unsigned char ch;
        if (i > mw1 || frag != 0x03) {
            ch = dw & 0x7F;
            if (ch != 0x03 && currentChar < (int)sizeof(message) - 1) {
                message[currentChar++] = ch;
            }
        }
        ch = (dw >> 7) & 0x7F;
        if (ch != 0x03 && currentChar < (int)sizeof(message) - 1) {
            message[currentChar++] = ch;
        }
        ch = (dw >> 14) & 0x7F;
        if (ch != 0x03 && currentChar < (int)sizeof(message) - 1) {
            message[currentChar++] = ch;
        }
    }
    message[currentChar] = '\0';

    // Strip trailing NULs / non-printable noise
    std::string content(message);
    while (!content.empty()) {
        unsigned char c = (unsigned char)content.back();
        if (c == 0 || c < 0x20) { content.pop_back(); }
        else { break; }
    }

    emit(self, f, phaseNo, content, fragmented, groupMessage);
}

static void parse_numeric(Decoder* self, Decoder::Impl* f,
                          const uint32_t* phaseptr, char phaseNo, int j)
{
    static const char flex_bcd[17] = "0123456789 U -][";

    int w1 = phaseptr[j] >> 7;
    int w2 = w1 >> 7;
    w1 = w1 & 0x7f;
    w2 = (w2 & 0x07) + w1;        // numeric message: max 7 words

    int dw;
    if (!f->long_address) {
        dw = phaseptr[w1];
        w1++;
        w2++;
    } else {
        dw = phaseptr[j + 1];
    }

    unsigned char digit = 0;
    int count = 4;
    if (f->page_type == FLEX_PAGETYPE_NUMBERED_NUMERIC) {
        count += 10;
    } else {
        count += 2;
    }
    std::string out;
    for (int i = w1; i <= w2 && i < 88; i++) {
        for (int k = 0; k < 21; k++) {
            digit = (digit >> 1) & 0x0F;
            if (dw & 0x01) { digit ^= 0x08; }
            dw >>= 1;
            if (--count == 0) {
                if (digit != 0x0C) {       // skip "fill"
                    out += flex_bcd[digit];
                }
                count = 4;
            }
        }
        dw = phaseptr[i];
    }

    emit(self, f, phaseNo, out, false, false);
}

static void parse_tone_only(Decoder* self, Decoder::Impl* f, char phaseNo) {
    emit(self, f, phaseNo, std::string(), false, false);
}

// "Unknown" / binary / short-instruction pages: we emit a short description
// rather than the raw binary so the user still sees the address activity.
static void parse_unknown(Decoder* self, Decoder::Impl* f, char phaseNo) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "<page type %d>", f->page_type);
    emit(self, f, phaseNo, std::string(buf), false, false);
}

static void parse_capcode(Decoder::Impl* f, uint32_t aw1) {
    f->long_address = (aw1 < 0x008001L) || (aw1 > 0x1E0000L) || (aw1 > 0x1E7FFEL);
    f->capcode = (int64_t)aw1 - 0x8000;
}

static void decode_phase(Decoder* self, Decoder::Impl* f, char phaseNo) {
    uint32_t* phaseptr = nullptr;
    switch (phaseNo) {
        case 'A': phaseptr = f->PhaseA; break;
        case 'B': phaseptr = f->PhaseB; break;
        case 'C': phaseptr = f->PhaseC; break;
        case 'D': phaseptr = f->PhaseD; break;
    }
    if (!phaseptr) return;

    // BCH-correct all 88 codewords; any uncorrectable one aborts the phase.
    for (int i = 0; i < 88; i++) {
        if (bch3121_fix_errors(&phaseptr[i]) != 0) {
            flog::debug("FLEX: Phase {} corrupt at block {}", (char)phaseNo, i);
            return;
        }
        phaseptr[i] &= 0x001FFFFF;
    }

    uint32_t biw = phaseptr[0];
    if (biw == 0 || biw == 0x001FFFFF) { return; }

    int voffset = (biw >> 10) & 0x3f;
    int aoffset = ((biw >> 8) & 0x03) + 1;

    if (voffset > 87 || aoffset > 87 || voffset <= aoffset) { return; }

    for (int i = aoffset; i < voffset; i++) {
        int j = voffset + i - aoffset;       // matching vector slot for address slot i
        if (j < 0 || j >= 88) { continue; }

        if (phaseptr[i] == 0x00000000 || phaseptr[i] == 0x001FFFFF) {
            continue;       // idle codewords
        }

        parse_capcode(f, phaseptr[i]);

        if (f->capcode > 4297068542ll || f->capcode < 0) {
            continue;
        }

        uint32_t viw = phaseptr[j];
        f->page_type = (int)((viw >> 4) & 0x07);
        int mw1 = (viw >> 7) & 0x7F;
        int len = (viw >> 14) & 0x7F;

        if (f->page_type == FLEX_PAGETYPE_SHORT_INSTRUCTION) {
            // The group-call list expansion that multimon-ng does is not
            // implemented here; we simply emit the short-instruction page
            // so the user knows it happened.
            parse_unknown(self, f, phaseNo);
            continue;
        }

        int mw2 = mw1 + (len - 1);
        if (mw1 == 0 && mw2 == 0) { continue; }
        if (is_tone(f->page_type)) { mw1 = mw2 = 0; }

        if (is_alpha(f->page_type)) {
            if (mw1 > 87 || mw2 > 87) { continue; }
            parse_alphanumeric(self, f, phaseptr, phaseNo, mw1, mw2, false);
        } else if (is_numeric(f->page_type)) {
            parse_numeric(self, f, phaseptr, phaseNo, j);
        } else if (is_tone(f->page_type)) {
            parse_tone_only(self, f, phaseNo);
        } else {
            parse_unknown(self, f, phaseNo);
        }
    }
}

static void clear_phase_data(Decoder::Impl* f) {
    std::memset(f->PhaseA, 0, sizeof(f->PhaseA));
    std::memset(f->PhaseB, 0, sizeof(f->PhaseB));
    std::memset(f->PhaseC, 0, sizeof(f->PhaseC));
    std::memset(f->PhaseD, 0, sizeof(f->PhaseD));
    f->PhaseA_idle = f->PhaseB_idle = f->PhaseC_idle = f->PhaseD_idle = 0;
    f->phase_toggle = 0;
    f->data_bit_counter = 0;
}

static void decode_data(Decoder* self, Decoder::Impl* f) {
    if (f->sync_baud == 1600) {
        if (f->sync_levels == 2) {
            decode_phase(self, f, 'A');
        } else {
            decode_phase(self, f, 'A');
            decode_phase(self, f, 'B');
        }
    } else {
        if (f->sync_levels == 2) {
            decode_phase(self, f, 'A');
            decode_phase(self, f, 'C');
        } else {
            decode_phase(self, f, 'A');
            decode_phase(self, f, 'B');
            decode_phase(self, f, 'C');
            decode_phase(self, f, 'D');
        }
    }
}

// Bit-by-bit packer that deinterleaves the data block into the 4 phases.
// 1:1 port of multimon-ng's read_data().
static int read_data(Decoder::Impl* f, unsigned char sym) {
    int bit_a = (sym > 1);
    int bit_b = 0;
    if (f->sync_levels == 4) {
        bit_b = (sym == 1) || (sym == 2);
    }
    if (f->sync_baud == 1600) {
        f->phase_toggle = 0;
    }
    unsigned int idx = ((f->data_bit_counter >> 5) & 0xFFF8u) |
                       (f->data_bit_counter & 0x0007u);
    if (idx >= 88) { return 0; }       // safety

    if (f->phase_toggle == 0) {
        f->PhaseA[idx] = (f->PhaseA[idx] >> 1) | (bit_a ? 0x80000000u : 0u);
        f->PhaseB[idx] = (f->PhaseB[idx] >> 1) | (bit_b ? 0x80000000u : 0u);
        f->phase_toggle = 1;
        if ((f->data_bit_counter & 0xFFu) == 0xFFu) {
            if (f->PhaseA[idx] == 0x00000000u || f->PhaseA[idx] == 0xFFFFFFFFu) f->PhaseA_idle++;
            if (f->PhaseB[idx] == 0x00000000u || f->PhaseB[idx] == 0xFFFFFFFFu) f->PhaseB_idle++;
        }
    } else {
        f->PhaseC[idx] = (f->PhaseC[idx] >> 1) | (bit_a ? 0x80000000u : 0u);
        f->PhaseD[idx] = (f->PhaseD[idx] >> 1) | (bit_b ? 0x80000000u : 0u);
        f->phase_toggle = 0;
        if ((f->data_bit_counter & 0xFFu) == 0xFFu) {
            if (f->PhaseC[idx] == 0x00000000u || f->PhaseC[idx] == 0xFFFFFFFFu) f->PhaseC_idle++;
            if (f->PhaseD[idx] == 0x00000000u || f->PhaseD[idx] == 0xFFFFFFFFu) f->PhaseD_idle++;
        }
    }

    if (f->sync_baud == 1600 || f->phase_toggle == 0) {
        f->data_bit_counter++;
    }

    int idle = 0;
    if (f->sync_baud == 1600) {
        if (f->sync_levels == 2) {
            idle = (f->PhaseA_idle > IDLE_THRESHOLD);
        } else {
            idle = (f->PhaseA_idle > IDLE_THRESHOLD) && (f->PhaseB_idle > IDLE_THRESHOLD);
        }
    } else {
        if (f->sync_levels == 2) {
            idle = (f->PhaseA_idle > IDLE_THRESHOLD) && (f->PhaseC_idle > IDLE_THRESHOLD);
        } else {
            idle = (f->PhaseA_idle > IDLE_THRESHOLD) && (f->PhaseB_idle > IDLE_THRESHOLD) &&
                   (f->PhaseC_idle > IDLE_THRESHOLD) && (f->PhaseD_idle > IDLE_THRESHOLD);
        }
    }
    return idle;
}

// Per-symbol state machine
static void flex_sym(Decoder* self, Decoder::Impl* f, unsigned char sym) {
    unsigned char sym_rectified = f->sync_polarity ? (3 - sym) : sym;

    switch (f->state_current) {
        case FLEX_STATE_SYNC1: {
            unsigned int sync_code = flex_sync(f, sym);
            if (sync_code != 0) {
                decode_mode(f, sync_code);
                if (f->sync_baud != 0 && f->sync_levels != 0) {
                    f->state_current = FLEX_STATE_FIW;
                } else {
                    f->state_current = FLEX_STATE_SYNC1;
                }
            } else {
                f->state_current = FLEX_STATE_SYNC1;
            }
            f->fiwcount = 0;
            f->fiw_rawdata = 0;
            break;
        }
        case FLEX_STATE_FIW: {
            f->fiwcount++;
            if (f->fiwcount >= 16) {
                read_2fsk(sym_rectified, &f->fiw_rawdata);
            }
            if (f->fiwcount == 48) {
                if (decode_fiw(f) == 0) {
                    f->sync2_count = 0;
                    f->demod_baud  = f->sync_baud;
                    f->state_current = FLEX_STATE_SYNC2;
                } else {
                    f->state_current = FLEX_STATE_SYNC1;
                }
            }
            break;
        }
        case FLEX_STATE_SYNC2: {
            if (++f->sync2_count == f->sync_baud * 25 / 1000) {
                f->data_count = 0;
                clear_phase_data(f);
                f->state_current = FLEX_STATE_DATA;
            }
            break;
        }
        case FLEX_STATE_DATA: {
            int idle = read_data(f, sym_rectified);
            if (++f->data_count == f->sync_baud * 1760 / 1000 || idle) {
                decode_data(self, f);
                f->demod_baud   = 1600;
                f->state_current = FLEX_STATE_SYNC1;
                f->data_count    = 0;
            }
            break;
        }
    }
}

// Symbol detector with adaptive envelope and phase lock.
// Returns 1 when a symbol has just been emitted, 0 otherwise.
static int buildSymbol(Decoder::Impl* f, double sample) {
    const int64_t phase_max  = 100LL * f->sample_freq;
    const int64_t phase_rate = phase_max * f->demod_baud / f->sample_freq;
    const double  phasepercent = 100.0 * f->phase / (double)phase_max;

    f->sample_count++;

    // DC offset removal during SYNC1 (IIR)
    if (f->state_current == FLEX_STATE_SYNC1) {
        f->zero = (f->zero * ((double)f->sample_freq * DC_OFFSET_FILTER) + sample) /
                  ((double)f->sample_freq * DC_OFFSET_FILTER + 1.0);
    }
    sample -= f->zero;

    if (f->locked) {
        if (f->state_current == FLEX_STATE_SYNC1) {
            f->envelope_sum += std::fabs(sample);
            f->envelope_count++;
            f->envelope = f->envelope_sum / (double)f->envelope_count;
        }
    } else {
        f->envelope        = 0;
        f->envelope_sum    = 0;
        f->envelope_count  = 0;
        f->demod_baud      = 1600;
        f->timeout         = 0;
        f->nonconsec       = 0;
        f->state_current   = FLEX_STATE_SYNC1;
    }

    // Mid 80% of symbol period: count level occurrences (4-FSK slicer)
    if (phasepercent > 10 && phasepercent < 90) {
        if (sample > 0) {
            if (sample > f->envelope * SLICE_THRESHOLD) f->symcount[3]++;
            else                                        f->symcount[2]++;
        } else {
            if (sample < -f->envelope * SLICE_THRESHOLD) f->symcount[0]++;
            else                                          f->symcount[1]++;
        }
    }

    // Zero-crossing detector for symbol-rate phase tracking
    if ((f->sample_last < 0 && sample >= 0) || (f->sample_last >= 0 && sample < 0)) {
        double phase_error = (phasepercent < 50) ? (double)f->phase
                                                 : (double)(f->phase - phase_max);
        // The correction must be applied in floating point: a small
        // phase_error multiplied by PHASE_*_RATE produces a fractional
        // value that would round to zero if cast to int before the
        // subtraction, defeating the phase-tracking loop entirely.
        double newPhase = (double)f->phase - phase_error *
                          (f->locked ? PHASE_LOCKED_RATE : PHASE_UNLOCKED_RATE);
        f->phase = (int)newPhase;

        if (phasepercent > 10 && phasepercent < 90) {
            f->nonconsec++;
            if (f->nonconsec > 20 && f->locked) {
                flog::debug("FLEX: Synchronisation Lost");
                f->locked = 0;
            }
        } else {
            f->nonconsec = 0;
        }
        f->timeout = 0;
    }
    f->sample_last = sample;

    f->phase += (int)phase_rate;
    if (f->phase > phase_max) {
        f->phase -= (int)phase_max;
        return 1;
    }
    return 0;
}

static void Flex_Demodulate(Decoder* self, Decoder::Impl* f, double sample) {
    if (buildSymbol(f, sample) == 1) {
        f->nonconsec = 0;
        f->symbol_count++;
        f->symbol_rate = 1.0 * f->symbol_count * f->sample_freq / (double)f->sample_count;

        int decmax = 0;
        int modal_symbol = 0;
        for (int j = 0; j < 4; j++) {
            if (f->symcount[j] > decmax) {
                modal_symbol = j;
                decmax = f->symcount[j];
            }
        }
#ifdef FLEX_TRACE
        fprintf(stderr, "sym# %3u modal=%d counts=[%d,%d,%d,%d] state=%d locked=%d\n",
                f->symbol_count, modal_symbol,
                f->symcount[0], f->symcount[1], f->symcount[2], f->symcount[3],
                (int)f->state_current, f->locked);
#endif
        f->symcount[0] = f->symcount[1] = f->symcount[2] = f->symcount[3] = 0;

        if (f->locked) {
            flex_sym(self, f, (unsigned char)modal_symbol);
        } else {
            f->lock_buf = (f->lock_buf << 2) | (uint64_t)(modal_symbol ^ 0x1);
            uint64_t lock_pattern = f->lock_buf ^ 0x6666666666666666ull;
            uint64_t lock_mask = (1ull << (2 * LOCK_LEN)) - 1;
            if ((lock_pattern & lock_mask) == 0 || ((~lock_pattern) & lock_mask) == 0) {
                flog::debug("FLEX: Locked");
                f->locked = 1;
                f->lock_buf = 0;
                f->symbol_count = 0;
                f->sample_count = 0;
            }
        }

        f->timeout++;
        if (f->timeout > DEMOD_TIMEOUT) {
            flog::debug("FLEX: Timeout");
            f->locked = 0;
        }
    }
}

// ---------------------------------------------------------------------
// Public class methods
// ---------------------------------------------------------------------
Decoder::Decoder(double sampleRate) : impl(new Impl()) {
    impl->sample_freq = (unsigned int)sampleRate;
    impl->demod_baud  = 1600;
    bch_init();
}

Decoder::~Decoder() { delete impl; }

void Decoder::process(const float* samples, int count) {
    if (!impl) return;
    for (int i = 0; i < count; i++) {
        Flex_Demodulate(this, impl, (double)samples[i]);
    }
}

void Decoder::reset() {
    if (!impl) return;
    unsigned int sf = impl->sample_freq;
    *impl = Impl();
    impl->sample_freq = sf;
    impl->demod_baud  = 1600;
}

bool Decoder::isLocked()                const { return impl && impl->locked != 0; }
unsigned int Decoder::currentSyncBaud()   const { return impl ? impl->sync_baud   : 0; }
unsigned int Decoder::currentSyncLevels() const { return impl ? impl->sync_levels : 0; }

} // namespace flex
