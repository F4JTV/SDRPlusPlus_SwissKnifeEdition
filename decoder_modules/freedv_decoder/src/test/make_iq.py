#!/usr/bin/env python3
"""
make_iq.py - turn a real FreeDV modem-audio WAV (8 kHz, the audio an SSB
receiver would produce) into a complex IQ WAV that SDR++ can open with its
"File" source, so the freedv_decoder module can be exercised end to end
(VFO -> SSB/USB demod -> FreeDV decode).

The modem audio is treated as an upper-sideband signal: we form its analytic
(Hilbert) signal, resample to the IQ sample rate, and shift it to a chosen
frequency offset from the recording centre. In SDR++: open the IQ file, move
the VFO to that offset, pick the matching mode, and you should get sync.

Usage:
    make_iq.py <in_modem.wav> <out_iq.wav> [offset_hz=6000] [iq_fs=48000]
"""
import sys
import numpy as np
from scipy.signal import hilbert, resample_poly
from scipy.io import wavfile


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    in_path = sys.argv[1]
    out_path = sys.argv[2]
    offset = float(sys.argv[3]) if len(sys.argv) > 3 else 6000.0
    iq_fs = int(sys.argv[4]) if len(sys.argv) > 4 else 48000

    fs, x = wavfile.read(in_path)
    if x.ndim > 1:
        x = x[:, 0]
    x = x.astype(np.float64)
    x /= (np.max(np.abs(x)) + 1e-9)

    # Analytic signal -> keep only the upper sideband (positive frequencies).
    analytic = hilbert(x)

    # Resample complex signal from fs to iq_fs (rational).
    from math import gcd
    g = gcd(iq_fs, fs)
    up, down = iq_fs // g, fs // g
    re = resample_poly(analytic.real, up, down)
    im = resample_poly(analytic.imag, up, down)
    sig = re + 1j * im

    # Shift to the requested offset within the IQ band.
    n = np.arange(len(sig))
    sig = sig * np.exp(1j * 2.0 * np.pi * offset * n / iq_fs)

    # Scale and pack as 16-bit stereo (I = left, Q = right).
    peak = np.max(np.abs(np.concatenate([sig.real, sig.imag]))) + 1e-9
    scale = 0.7 * 32767.0 / peak
    iq = np.empty((len(sig), 2), dtype=np.int16)
    iq[:, 0] = np.round(sig.real * scale).astype(np.int16)
    iq[:, 1] = np.round(sig.imag * scale).astype(np.int16)

    wavfile.write(out_path, iq_fs, iq)
    print(f"wrote {out_path}: {len(sig)} samples @ {iq_fs} Hz IQ, "
          f"FreeDV signal (USB) at +{offset:.0f} Hz from centre")


if __name__ == "__main__":
    main()
