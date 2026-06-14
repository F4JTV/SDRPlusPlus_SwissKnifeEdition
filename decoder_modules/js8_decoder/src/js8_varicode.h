/*
 * js8_varicode.h - JS8 message-token interpretation (no Qt, no SDR++).
 *
 * Ported from JS8Call's Varicode.cpp (GPL-3.0). Turns the CRC-valid 12-char
 * token + 3-bit frame type produced by js8_core into a best-effort
 * human-readable message.
 *
 * Coverage:
 *   - heartbeat / compound / compound-directed frames (callsign + grid / cmd)
 *   - directed frames (from : to cmd [num])
 *   - free-text data frames via the built-in default Huffman table
 *
 * NOT covered (falls back to the raw token): JSC-compressed data frames, which
 * require the multi-megabyte JSC dictionary that is intentionally not vendored
 * here.
 */
#pragma once

#include <string>

namespace js8 {

// Produce a human-readable interpretation of a decoded token.
//  token : the 12-character payload (over the 64-symbol JS8 alphabet)
//  i3    : the 3-bit frame type as transmitted (see FrameType in js8_core.h)
// Returns a readable string, or the raw token if it cannot be interpreted.
std::string interpretMessage(const std::string& token, int i3);

} // namespace js8
