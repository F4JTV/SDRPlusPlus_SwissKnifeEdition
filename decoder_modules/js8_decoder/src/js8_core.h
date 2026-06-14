/*
 * js8_core.h - Self-contained JS8 encode/decode core for SDR++ (no Qt).
 *
 * Ported from the JS8Call / JS8Call-improved project (GPL-3.0), itself a
 * derivative of WSJT-X. The JS8 protocol and the JS8 modulation were created
 * by Jordan Sherer (KN4CRD). The LDPC(174,87) code, CRC-12 framing, Costas
 * arrays, 6-bit message alphabet and reference-signal generation are ported
 * from JS8Call's "JS8.cpp". The belief-propagation decoder, parity matrices
 * and message-extraction logic are taken from the same source.
 *
 * This file exposes a minimal C++ interface so the algorithm can run inside an
 * SDR++ decoder module and inside a standalone generator / test harness,
 * without any Qt or SDR++ dependency.
 *
 * Only "Normal" submode (15 s, 6.25 Hz tone spacing, ORIGINAL Costas) is wired
 * up end-to-end here; the framing constants for the other submodes are present
 * so the engine can be extended.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace js8 {

// --- Audio / framing constants (Normal submode) -----------------------------
constexpr int    SAMPLE_RATE   = 12000;  // decoder/encoder audio sample rate
constexpr int    NSPS_NORMAL   = 1920;   // samples per symbol (Normal)
constexpr double DF_NORMAL     = 6.25;   // tone spacing in Hz (Normal)
constexpr int    NUM_SYMBOLS   = 79;     // total channel symbols
constexpr int    NUM_TONES     = 8;      // 8-FSK

// --- Frame type (i3, low 3 bits of the message type) ------------------------
// 000 heartbeat | 001 compound | 010 compound-directed | 011 directed
// 1XX data (free text), 11X data-compressed
enum FrameType {
    FrameHeartbeat        = 0,
    FrameCompound         = 1,
    FrameCompoundDirected = 2,
    FrameDirected         = 3,
    FrameData             = 4,
    FrameDataCompressed   = 6,
    FrameUnknown          = 255
};

// One successful (CRC-valid) decode.
struct Decode {
    int         i3        = 0;     // 3-bit frame type as transmitted
    std::string token;            // 12-character 6-bit-alphabet payload
    std::string text;             // best-effort human-readable message
    float       f0        = 0.0f; // base (lowest-tone) audio frequency, Hz
    float       dt        = 0.0f; // time offset within the slot, seconds
    float       snr       = 0.0f; // estimated SNR (approx, JS8-ish scale)
    int         harderrors = 0;   // LDPC hard-decision errors (decode quality)
};

// Decode one analysis window of REAL audio at 12 kHz (Normal submode).
// Searches base frequencies in [fMin, fMax]. Returns all CRC-valid decodes,
// de-duplicated by token+frequency.
std::vector<Decode> decodeNormal(const float* audio, std::size_t n,
                                 float fMin = 200.0f, float fMax = 2800.0f);

// --- Encoder / generator (for the test generator) ---------------------------

// Pack a 12-character token (over the 64-symbol JS8 alphabet) plus a 3-bit
// frame type into 79 tone indices using the ORIGINAL (Normal) Costas arrays.
// Throws std::runtime_error on an invalid character.
void encodeNormal(const std::string& token12, int i3type, int tones[NUM_SYMBOLS]);

// Render 79 tone indices to REAL audio at 12 kHz using continuous-phase FSK
// (optionally Gaussian-smoothed). f0 is the lowest-tone frequency in Hz.
// startDelaySec of leading silence is prepended; trailing padding is appended
// so the buffer covers a full 15 s slot. gfsk enables GFSK pulse shaping
// (closer to a real JS8 transmitter); when false a pure CPFSK reference tone
// is produced (matches JS8Call's genjs8refsig).
std::vector<float> genAudioNormal(const int tones[NUM_SYMBOLS], float f0,
                                  float startDelaySec = 0.5f, bool gfsk = true);

// Convenience: 12-char token <-> readable interpretation helpers live in
// js8_varicode.h. The core only guarantees the CRC-valid token + i3.

} // namespace js8
