# MT63 decoder - test tools

Standalone command-line tools that exercise the same Jalocha MT63 engine the
module uses, for validating the decoder without a live radio. They depend only
on the vendored engine under `../src/mt63/jalocha` (and, for the WAV decoder,
the header-only modem wrapper under `../src/mt63`), so no SDR++ build is needed.

Build all three:

```
g++ -O2 -std=c++17 -I ../src/mt63/jalocha \
    mt63_gen.cpp       ../src/mt63/jalocha/mt63base.cpp ../src/mt63/jalocha/dsp.cpp -o mt63_gen
g++ -O2 -std=c++17 -I ../src/mt63/jalocha \
    mt63_roundtrip.cpp ../src/mt63/jalocha/mt63base.cpp ../src/mt63/jalocha/dsp.cpp -o mt63_roundtrip
g++ -O2 -std=c++17 -I ../src \
    mt63_decode_wav.cpp ../src/mt63/jalocha/mt63base.cpp ../src/mt63/jalocha/dsp.cpp -o mt63_decode_wav
```

## mt63_gen - signal generator

Encodes text into an MT63 WAV (8 kHz, mono, 16-bit) with an optional noise
level. This is the generator to use when no on-air signal is available. The
lead-in idle defaults to the full interleaver depth plus a sync-acquisition
margin, so even a short message survives MT63's acquisition transient.

```
./mt63_gen --mode 1000S "CQ CQ DE TEST 73"
./mt63_gen --mode 2000L --snr 3 --center 1500 "EMCOMM NET CHECK-IN"
./mt63_gen --mode 500L  --out weak.wav --snr -2 "weak signal test"
```

Options: `--mode m` (`500S 500L 1000S 1000L 2000S 2000L`, default `1000S`),
`--center Hz` (default 1500), `--snr dB` (in-band SNR of added white noise; omit
for clean), `--lead sec` (lead-in idle; omit for auto), `--tail sec` (idle tail
to flush the interleaver; omit for auto), `--out file`, `--seed N`.

## mt63_roundtrip - automated self-test

Encodes a known message, optionally adds noise, decodes it back and reports
pass/fail across all six submodes and several SNRs. No arguments:

```
./mt63_roundtrip
```

The check requires the message tail to survive, which is what matters in
practice: MT63 always loses the first several seconds of any transmission to
sync acquisition and interleaver fill.

## mt63_decode_wav - decode a WAV file

Decodes a WAV recording **through the module's own modem wrapper**
(`../src/mt63/modem.h`), i.e. the exact code path the live module runs, just
driven from a file. The loader accepts any PCM/float WAV: it parses the chunk
list, downmixes to mono and linear-resamples to 8 kHz, so a raw off-air
recording (typically 44.1/48 kHz stereo) can be fed in directly.

```
./mt63_decode_wav file.wav [mode|scan] [center|scan]
./mt63_decode_wav sample.wav 1000L 1500      # mode and centre known
./mt63_decode_wav sample.wav                 # blind: scan modes and centres
```

When `mode`/`center` are omitted (or `scan`), the tool sweeps the six submodes
and a coarse centre grid and prints the best decode (most printable characters,
sync confidence breaking ties). The leading characters of any decode are the
expected acquisition transient; the real text follows once sync locks.

### Real off-air samples

Off-air MT63 recordings (500/1000/2000) are published on the
Signal Identification Wiki MT63 page (linked sound clips hosted on
`www.pa4rm.com`). Convert one to the expected format first, e.g.:

```
ffmpeg -i 070_mt63_1000.mp3 -ac 1 -ar 8000 mt63_1000.wav
./mt63_decode_wav mt63_1000.wav 1000L scan
```
