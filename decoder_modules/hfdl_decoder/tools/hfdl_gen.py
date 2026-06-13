#!/usr/bin/env python3
"""
hfdl_gen.py - synthetic HFDL burst generator with variable noise.

Generates a baseband HFDL signal and writes it as interleaved 16-bit I/Q
(CS16) at 12 kHz - the exact format the SDR++ HFDL module hands to dumphfdl,
and what `dumphfdl --iq-file - --sample-format CS16 --sample-rate 12000` reads.

What it reproduces faithfully (from dumphfdl/src/hfdl.c):
  * 1800 Bd symbol rate, root-raised-cosine pulse shaping.
  * The real acquisition preamble:
        PREKEY(448) + A(127) + A(127) + M1(127) + M2(15) + 9 x T(15)
    using the genuine A octets, M1 bit sequence, M-shifts and T training
    symbols dumphfdl correlates against (CORR_THRESHOLD_A1/A2/M1).
  * Upper-sideband geometry: the modulation is placed at +1440 Hz
    (HFDL_SSB_CARRIER_OFFSET_HZ) so that tuning to the channel centre and
    letting dumphfdl mix it down works exactly as for an off-air recording.
  * BPSK / PSK4 / PSK8 payload symbols and additive white Gaussian noise at a
    user-selected Es/N0.

Scope and honesty: this exercises the *signal path* and dumphfdl's burst
*detection* (preamble correlation -> A1/A2/M1 lock). It does NOT build a
green, fully decodable frame: HFDL's scrambler, convolutional code,
interleaver and Reed-Solomon outer code on the payload are not synthesised, so
dumphfdl will detect and attempt the burst but report payload/CRC failures.
That is the right tool for validating tuning, sample-format, channelisation
and the fork/exec/pipe path end to end. To observe detection directly, build
dumphfdl with -DDEBUG and watch A1_found / A2_found / M1_found increment.

Usage examples:
  ./hfdl_gen.py -o /tmp/hfdl.cs16 --snr 20 --bursts 3
  ./hfdl_gen.py -o /tmp/hfdl.cs16 --snr 6  --scheme psk4
  ./hfdl_gen.py -o /tmp/hfdl.cf32 --format cf32 --snr 100   # clean reference
  dumphfdl --iq-file /tmp/hfdl.cs16 --sample-format CS16 \
           --sample-rate 12000 --centerfreq 11384 11384 \
           --output decoded:text:file:path=-
"""

import argparse
import sys
import numpy as np

# ---------------------------------------------------------------------------
# HFDL physical-layer constants (mirrors dumphfdl/src/hfdl.{c,h})
# ---------------------------------------------------------------------------
SYMBOL_RATE = 1800
SAMPLE_RATE = 12000
CARRIER_OFFSET_HZ = 1440          # HFDL_SSB_CARRIER_OFFSET_HZ (upper sideband)

PREKEY_LEN = 448
A_LEN = 127
M1_LEN = 127
M2_LEN = 15
T_LEN = 15

# A sequence (127 bits packed into 16 octets, MSB first) - A_octets[] in hfdl.c
A_OCTETS = [
    0b01011011, 0b10111100, 0b01110100, 0b01010111,
    0b00000011, 0b11011001, 0b10001001, 0b00111001,
    0b11110010, 0b00001000, 0b11010101, 0b00110110,
    0b10010100, 0b00101100, 0b00110010, 0b11111110,
]

# M1 maximal-length-ish sequence - M1_bits[] in hfdl.c
M1_BITS = [
    0,1,1,1,0,1,1,0,1,1,1,1,0,1,0,0,0,1,0,1,1,0,0,
    1,0,1,1,1,1,1,0,0,0,1,0,0,0,0,0,0,1,1,0,0,1,1,0,1,1,
    0,0,0,1,1,1,0,0,1,1,1,0,1,0,1,1,1,0,0,0,0,1,0,0,1,1,
    0,0,0,0,0,1,0,1,0,1,0,1,1,0,1,0,0,1,0,0,1,0,1,0,0,1,
    1,1,1,0,0,1,0,0,0,1,1,0,1,0,1,0,0,0,0,1,1,1,1,1,1,1,
]

M_SHIFTS = [72, 82, 113, 123, 61, 103, 93, 9]

# Training symbols T_seq[0] (BPSK, already +/-1) - T_seq[][] in hfdl.c
T_SEQ0 = [1, 1, 1, -1, 1, 1, -1, -1, 1, -1, 1, -1, -1, -1, -1]


def unpack_a_bits():
    """Expand A_OCTETS (MSB first) into the first A_LEN bits."""
    bits = []
    for octet in A_OCTETS:
        for k in range(7, -1, -1):
            bits.append((octet >> k) & 1)
    return bits[:A_LEN]


def bits_to_bpsk(bits, polarity=1):
    """Map {0,1} -> {-1,+1} (polarity=+1) or {+1,-1} (polarity=-1)."""
    arr = np.asarray(bits, dtype=np.float64)
    sym = (2.0 * arr - 1.0) * polarity
    return sym.astype(np.complex128)


def rrc_taps(beta, span_syms, sps):
    """Root-raised-cosine filter, unit-energy, sampled at `sps` samples/symbol."""
    n = np.arange(-span_syms * sps, span_syms * sps + 1, dtype=np.float64)
    t = n / sps
    h = np.zeros_like(t)
    for i, ti in enumerate(t):
        if abs(ti) < 1e-12:
            h[i] = 1.0 - beta + 4.0 * beta / np.pi
        elif beta > 0 and abs(abs(ti) - 1.0 / (4.0 * beta)) < 1e-9:
            h[i] = (beta / np.sqrt(2.0)) * (
                (1 + 2 / np.pi) * np.sin(np.pi / (4 * beta)) +
                (1 - 2 / np.pi) * np.cos(np.pi / (4 * beta)))
        else:
            num = (np.sin(np.pi * ti * (1 - beta)) +
                   4 * beta * ti * np.cos(np.pi * ti * (1 + beta)))
            den = np.pi * ti * (1 - (4 * beta * ti) ** 2)
            h[i] = num / den
    h /= np.sqrt(np.sum(h ** 2))
    return h


def build_preamble_symbols(polarity, m1_index=0):
    """PREKEY + A + A + M1 + M2 + 9 x T as a complex BPSK symbol vector.

    dumphfdl identifies the frame's modulation/rate by correlating the received
    M1 field against the M1 bit sequence rotated by each of the M_SHIFTS; the
    winning index selects hfdl_frame_params[]. We therefore transmit M1 (and the
    M2 tail) rotated by M_SHIFTS[m1_index] so the M1 correlator locks on that
    index instead of seeing an unrecognised (unrotated) sequence.
    """
    rng = np.random.default_rng(4471)
    # PREKEY: not correlated for detection (no PREKEY threshold in dumphfdl),
    # so a deterministic BPSK PN keeps AGC/timing busy without affecting lock.
    prekey = bits_to_bpsk(rng.integers(0, 2, PREKEY_LEN), polarity)

    a_bits = unpack_a_bits()
    a_sym = bits_to_bpsk(a_bits, polarity)

    shift = M_SHIFTS[m1_index]
    m1_bits = [M1_BITS[(shift + j) % M1_LEN] for j in range(M1_LEN)]
    m1_sym = bits_to_bpsk(m1_bits, polarity)

    m2_bits = [M1_BITS[(shift + j) % M1_LEN] for j in range(M2_LEN)]
    m2_sym = bits_to_bpsk(m2_bits, polarity)

    t_sym = np.asarray(T_SEQ0, dtype=np.complex128)
    t_block = np.tile(t_sym, 9)

    return np.concatenate([prekey, a_sym, a_sym, m1_sym, m2_sym, t_block])


def random_payload_symbols(n, scheme, rng):
    """n payload symbols for BPSK / PSK4 / PSK8 (random, uncoded)."""
    if scheme == "bpsk":
        m = 2
    elif scheme == "psk4":
        m = 4
    else:
        m = 8
    idx = rng.integers(0, m, n)
    phases = 2.0 * np.pi * idx / m
    return np.exp(1j * phases).astype(np.complex128)


def main():
    ap = argparse.ArgumentParser(description="Synthetic HFDL burst generator")
    ap.add_argument("-o", "--output", required=True, help="output file path")
    ap.add_argument("--format", choices=["cs16", "cf32"], default="cs16",
                    help="sample format (default cs16)")
    ap.add_argument("--snr", type=float, default=20.0,
                    help="Es/N0 in dB applied to the burst (default 20)")
    ap.add_argument("--scheme", choices=["bpsk", "psk4", "psk8"], default="psk4",
                    help="payload modulation (preamble is always BPSK)")
    ap.add_argument("--payload-syms", type=int, default=2160,
                    help="number of payload symbols per burst (default 2160 = "
                         "72 data frames x 30)")
    ap.add_argument("--bursts", type=int, default=1, help="number of bursts")
    ap.add_argument("--gap-ms", type=float, default=200.0,
                    help="silence between/around bursts in ms (default 200)")
    ap.add_argument("--beta", type=float, default=0.35, help="RRC roll-off")
    ap.add_argument("--polarity", type=int, choices=[1, -1], default=1,
                    help="BPSK bit->symbol polarity (default 1)")
    ap.add_argument("--m1-index", type=int, default=0, choices=range(8),
                    metavar="0-7",
                    help="M1 frame-parameter index to signal (default 0)")
    ap.add_argument("--seed", type=int, default=1234, help="PRNG seed")
    args = ap.parse_args()

    rng = np.random.default_rng(args.seed)

    # --- one burst of symbols: preamble + payload --------------------------
    preamble = build_preamble_symbols(args.polarity, args.m1_index)
    payload = random_payload_symbols(args.payload_syms, args.scheme, rng)
    burst_syms = np.concatenate([preamble, payload])

    # --- pulse shaping at 12 kHz via rational resampling 20/3 --------------
    # 12000 / 1800 = 20/3.  Upsample symbols by 20, RRC filter, /3 -> 12 kHz.
    UP, DOWN = 20, 3
    h = rrc_taps(args.beta, span_syms=6, sps=UP)
    try:
        from scipy.signal import upfirdn
        shaped = upfirdn(h, burst_syms, up=UP, down=DOWN)
    except ImportError:
        # Fallback: manual upsample-filter-decimate (slower, no SciPy needed).
        up = np.zeros(len(burst_syms) * UP, dtype=np.complex128)
        up[::UP] = burst_syms
        shaped = np.convolve(up, h)[::DOWN]

    # Normalise burst to unit average power before adding noise.
    shaped /= np.sqrt(np.mean(np.abs(shaped) ** 2))

    # --- assemble bursts with gaps -----------------------------------------
    gap = np.zeros(int(args.gap_ms * 1e-3 * SAMPLE_RATE), dtype=np.complex128)
    pieces = [gap]
    for _ in range(args.bursts):
        pieces.append(shaped.copy())
        pieces.append(gap)
    sig = np.concatenate(pieces)

    # --- shift to +1440 Hz (upper sideband) --------------------------------
    n = np.arange(len(sig))
    sig = sig * np.exp(1j * 2.0 * np.pi * CARRIER_OFFSET_HZ * n / SAMPLE_RATE)

    # --- additive white Gaussian noise at requested Es/N0 ------------------
    # Noise is added across the whole stream; SNR refers to the active burst,
    # whose power was normalised to 1.0 above.
    snr_lin = 10.0 ** (args.snr / 10.0)
    noise_p = 1.0 / snr_lin
    noise = (np.sqrt(noise_p / 2.0) *
             (rng.standard_normal(len(sig)) + 1j * rng.standard_normal(len(sig))))
    sig = sig + noise

    # --- write out ---------------------------------------------------------
    if args.format == "cf32":
        inter = np.empty(len(sig) * 2, dtype=np.float32)
        inter[0::2] = sig.real.astype(np.float32)
        inter[1::2] = sig.imag.astype(np.float32)
        inter.tofile(args.output)
    else:
        peak = np.max(np.abs(np.concatenate([sig.real, sig.imag])))
        scale = (0.9 * 32767.0) / peak if peak > 0 else 1.0
        i16 = np.empty(len(sig) * 2, dtype=np.int16)
        i16[0::2] = np.clip(np.round(sig.real * scale), -32768, 32767).astype(np.int16)
        i16[1::2] = np.clip(np.round(sig.imag * scale), -32768, 32767).astype(np.int16)
        i16.tofile(args.output)

    dur = len(sig) / SAMPLE_RATE
    print(f"wrote {args.output}  format={args.format}  {len(sig)} samples "
          f"({dur:.2f} s)  bursts={args.bursts}  scheme={args.scheme}  "
          f"snr={args.snr} dB", file=sys.stderr)


if __name__ == "__main__":
    main()
