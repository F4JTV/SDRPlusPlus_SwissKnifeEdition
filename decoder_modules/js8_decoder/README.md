# JS8 decoder module for SDR++

A self-contained SDR++ decoder module for the **JS8** amateur-radio digital
mode. It demodulates a USB audio window, slices it into UTC-aligned time slots,
and decodes the JS8 *Normal* submode (15 s, 8-FSK, 6.25 Hz tone spacing) using
a from-scratch port of the JS8 LDPC(174,87) + CRC-12 framing. Decoded traffic
is shown in a detached results window and can optionally be streamed as JSON
lines over TCP.

The JS8 protocol and modulation were designed by Jordan Sherer (KN4CRD). The
encode/decode core in this module is ported from the open-source JS8Call
project (a derivative of WSJT-X) and is licensed under the GPL-3.0.

## Features

- JS8 **Normal** submode decoded end-to-end (sync search, LDPC belief-
  propagation decode, CRC-12 validation, message extraction).
- Message interpretation for the common frame types: heartbeat, compound and
  compound-directed (callsign + grid / command), directed (from / to / command
  / SNR), and free-text data frames via the built-in Huffman table.
- Detached, non-modal results window that never disturbs the waterfall VFO.
- Optional TCP JSON output to a map/aggregation backend.
- A standalone signal generator and a roundtrip test harness for validation
  when no off-air sample is available.

## Audio chain

```
VFO (complex baseband)
  -> dsp::demod::SSB<float>  (USB, 3 kHz window)
  -> 12 kHz real audio
  -> UTC slot-aligned capture (15 s)
  -> worker thread -> js8::decodeNormal()
```

The DSP blocks are initialised exactly once, in the constructor; enabling or
disabling the module only starts/stops the chain and re-attaches the VFO. The
decode worker thread lives for the whole module lifetime and blocks on the
slot queue when idle. **The PC clock must be NTP-synchronised** for the slot
alignment (and therefore decoding) to work.

## Building (in-tree)

This is an in-tree SDR++ module. Place the directory under `decoder_modules/`
in your SDR++ source tree:

```
decoder_modules/js8_decoder/
  CMakeLists.txt
  src/
    main.cpp
    js8_core.h      js8_core.cpp
    js8_varicode.h  js8_varicode.cpp
    js8_ldpc.h
    tcp_sender.h
```

Add the build option and subdirectory in the **root** `CMakeLists.txt`:

```cmake
option(OPT_BUILD_JS8_DECODER "Build the JS8 decoder module" ON)
# ...
if (OPT_BUILD_JS8_DECODER)
    add_subdirectory("decoder_modules/js8_decoder")
endif()
```

Then build SDR++ as usual:

```sh
cd build
cmake .. -DOPT_BUILD_JS8_DECODER=ON
make -j$(nproc)
```

Enable the module from **Module Manager** in the SDR++ UI (instance name e.g.
`JS8 Decoder`), then tune to a JS8 frequency in USB.

## Using the module

1. Tune the SDR to a JS8 dial frequency (the audio window spans roughly
   100–3000 Hz above the dial).
2. Open the module's menu entry. Choose the VFO **Snap** interval (1 Hz is a
   good default for fine tuning).
3. Click **Show Decodes** to open the results window. Each decode lists the UTC
   slot time, estimated SNR (dB), time offset (DT, s), audio frequency (Hz) and
   the interpreted message.
4. The results window has **Clear**, **Save TSV** and **Auto-scroll** controls.
   The TSV snapshot is written to the module's per-instance work directory.

## TCP / JSON output

Tick **Enable TCP** to stream each decode as one newline-terminated JSON object
to the configured host/port (default `127.0.0.1:10100`). The TCP worker is
non-blocking with a bounded queue (oldest dropped on overflow) and reconnects
automatically. The enabled state is intentionally **not** persisted — TCP is
always off at startup — while the host and port are remembered.

Each line follows the map-backend schema:

```json
{"name":"<message>","date":"YYYY-MM-DD","time":"HH:MM:SS","lat":null,"lon":null,"type":"JS8","speed":null,"info":"freq=1500;dt=0.5;snr=-8;i3=0;type=HB;token=000AANR58.."}
```

`lat`/`lon`/`speed` are `null` for JS8; all decode detail is carried in the
flat `info` string.

## Validation tools

Because ready-to-decode JS8 WAV samples are hard to find online, two standalone
tools are provided to exercise the decoder against a known transmitted token.

**Generator** — render a token to a 15 s, 12 kHz WAV with optional AWGN:

```sh
g++ -std=c++17 -O2 -Isrc tools/js8gen.cpp src/js8_core.cpp -o js8gen
./js8gen 0A1B2C3D4E5F out.wav 1500 -12 0
#         token        file    f0   snr i3
```

**Roundtrip harness** — encode, synthesise, add noise across a range of SNRs,
and verify exact token recovery:

```sh
g++ -std=c++17 -O2 -Isrc tools/js8test.cpp src/js8_core.cpp src/js8_varicode.cpp -o js8test
./js8test
```

The harness reliably recovers the exact transmitted token from clean audio and
remains solid well into negative audio SNR, as expected for an LDPC-coded
weak-signal mode.

## Scope and limitations

- Only the **Normal** submode is wired end-to-end. The framing constants for
  the Fast / Turbo / Slow / Ultra submodes (and their modified Costas arrays)
  are present in the core so the engine can be extended.
- The SNR figure is an estimate, referenced to a 2500 Hz noise bandwidth in the
  spirit of WSJT-X; treat it as indicative.
- **JSC-compressed** free-text data frames are not decompressed (that requires
  the multi-megabyte JSC dictionary, which is intentionally not vendored). Such
  frames fall back to showing the raw 12-character token. Uncompressed
  free-text data frames are decoded via the built-in Huffman table.
- On-air interoperability against live JS8Call traffic should be validated with
  real signals; the internal roundtrip proves the encoder, LDPC/CRC framing and
  decoder front-end are self-consistent.

## Credits and license

- JS8 protocol and modulation: Jordan Sherer, KN4CRD.
- Decode/encode core ported from JS8Call, a derivative of WSJT-X.
- Licensed under the **GNU General Public License v3.0** (see `LICENSE`).

Author: SDR++ Community.
