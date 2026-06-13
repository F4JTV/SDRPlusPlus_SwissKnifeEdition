# Contestia decoder - test tools

Standalone command-line tools that exercise the same Jalocha MFSK engine the
module uses (with `bContestia = true`), for validating the decoder without a
live radio. They depend only on the vendored engine under
`../src/contestia/jalocha`, so no SDR++ build is needed.

Build them all:

```
g++ -O2 -std=c++17 -I ../src/contestia contestia_gen.cpp        -o contestia_gen
g++ -O2 -std=c++17 -I ../src/contestia contestia_roundtrip.cpp  -o contestia_roundtrip
g++ -O2 -std=c++17 -I ../src/contestia contestia_decode_wav.cpp -o contestia_decode_wav
```

## contestia_gen - signal generator

Encodes text into a Contestia WAV (8 kHz, mono, 16-bit) with an optional noise
level. This is the generator to use when no on-air signal is available.

```
./contestia_gen --mode 16-500 "CQ CQ DE TEST 73"
./contestia_gen --mode 8-250 --snr 0 --center 1200 "weak signal test"
./contestia_gen --mode 32-1000 --out test.wav "hello world"
```

Options: `--mode tones-bw` (e.g. 4-125, 16-500, 64-2000), `--center Hz`
(default 1500), `--snr dB` (in-band SNR of added white noise; omit for clean),
`--lead sec` (lead-in idle for sync, default 1.0), `--out file`, `--seed N`.

Note Contestia transmits an uppercase-only 6-bit alphabet, so lowercase input is
folded to uppercase on encode (as in fldigi).

## contestia_roundtrip - automated self-test

Encodes a known message, optionally adds noise, decodes it back and reports
pass/fail across all 19 submodes and a range of SNRs. No arguments:

```
./contestia_roundtrip
```

## contestia_decode_wav - decode a WAV file

Decodes a mono 8 kHz 16-bit WAV, scanning candidate centre frequencies and
printing the best result (scored by text-likeness, since a wrong centre still
emits printable punctuation). Useful for real recordings (convert first, e.g.
`ffmpeg -i sample.mp3 -ac 1 -ar 8000 sample.wav`).

```
./contestia_decode_wav sample.wav <tones> <bandwidth>
./contestia_decode_wav contestia_16-500.wav 16 500
```

Note: the centre must land within the synchroniser's tolerance
(about +/- SyncMargin x toneSpacing). In the SDR++ module you place the centre
interactively with the band view and the f/o read-out, which avoids any
mis-lock.
