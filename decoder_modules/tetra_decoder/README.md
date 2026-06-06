# sdrpp-tetra-demodulator (UI overhaul + in-tree build, codec pre-bundled)

TETRA demodulator plugin for SDR++. Designed to fully demodulate and decode
TETRA downlink signals.

This package adds:
- Cleaner, sectioned UI (collapsing headers, LED indicators, status pills,
  framed timeslot grid with per-slot encryption badges, big frame counters).
- A `CMakeLists.txt` that works both **in-tree** (inside SDR++ source tree)
  and **out-of-tree** (standalone, via the `sdrpp_module.cmake` macro), with
  no arguments to pass — the mode is auto-detected.
- **The ETSI TETRA speech codec is already extracted and patched** under
  `src/decoder/codec/`, so you do NOT need to run `download_and_patch.sh`
  yourself. Just `cmake` and `make`.

Signal chain:
`VFO -> Demodulator (AGC -> FLL -> RRC -> ML timing recovery -> Costas)
-> Constellation -> Symbol extractor -> Differential decoder -> Bit unpacker
-> osmo-tetra decoder -> Sink`

---

## 1. Building in-tree (recommended)

### 1.1 Place the module

```bash
git clone https://github.com/AlexandreRouma/SDRPlusPlus
cd SDRPlusPlus/decoder_modules
tar xzf /path/to/sdrpp-tetra-demodulator-improved.tar.gz
mv sdrpp-tetra-demodulator tetra_demodulator
```

### 1.2 Register the option in SDR++

Edit the top-level `SDRPlusPlus/CMakeLists.txt`.

**a)** In the *Decoders* options block near the top, add:

```cmake
option(OPT_BUILD_TETRA_DEMODULATOR "Build the TETRA demodulator module (no extra deps)" ON)
```

**b)** Lower in the same file, in the second *Decoders* section that contains
the `add_subdirectory(...)` calls, add:

```cmake
if (OPT_BUILD_TETRA_DEMODULATOR)
    add_subdirectory("decoder_modules/tetra_demodulator")
endif (OPT_BUILD_TETRA_DEMODULATOR)
```

### 1.3 Build

```bash
cd SDRPlusPlus
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
sudo make install
```

After installation, open SDR++ and add the module via the **Module Manager**.

---

## 2. Building out-of-tree (standalone)

```bash
tar xzf sdrpp-tetra-demodulator-improved.tar.gz
cd sdrpp-tetra-demodulator
mkdir build && cd build
cmake ..
make -j$(nproc)
sudo make install
```

Then enable the new module via the SDR++ Module Manager.

---

## 3. UI overview

When the plugin is enabled, the menu is organised in collapsing sections:

- **Demodulator** — constellation diagram, sync LED, signal-quality meter
  with inline percentage.
- **Output mode** — radio buttons for `OSMO-TETRA` (built-in decoder +
  audio) or `NETSYMS` (raw symbol streaming over UDP for `tetra-rx`).
- **Decoder** (OSMO-TETRA mode) — locked/unlocked LED, big
  `HYPERFRAME / MULTIFRAME / FRAME` counters, framed timeslot grid
  (TS1..TS4 colour-coded by content, with E1/E2/E3 amber badges on slots
  carrying encrypted traffic), per-slot SSI grid with call-state dot
  and freshness fade, last-CRC status.
- **Cell info** — MCC / MNC / CC laid out in 3 columns, DL/UL frequencies
  with usage tags, access codes.
- **Services & capabilities** — pill-style badges (green/grey) for Adv.
  link, Encryption, Voice, Circuit data, SNDCP, Normal mode, Migration,
  Never min., Priority cell, Dereg. req., Reg. req.
- **Network output** (NETSYMS mode) — training-sequence LED, UDP
  destination host/port input, Start/Stop button, UDP active/idle LED.
- **Data export** — TCP feed (default `127.0.0.1:10100`) carrying ONLY
  LIP/GPS positions in the unified `type:"TETRA"` JSON schema (same as
  AIS / ADS-B / APRS / DSD-FME / radiosonde). Background worker with
  automatic reconnect (1s → 2s → 4s → 8s → 16s backoff) and bounded ring
  buffer. Calls, SDS text messages and status codes live in the floating
  TETRA Data window opened from this section.

---

## 4. Usage

1. Find a TETRA downlink frequency.
2. Drop the demodulator VFO on the centre of the carrier.
3. The decoder typically locks once SNR exceeds ~20 dB; you should see
   four constellation clusters and the Sync LED turn green.
4. If the channel is unencrypted, voice will play out automatically. To
   feed symbols to `tetra-rx`, switch the output mode to `NETSYMS`, fill
   in host/port and click **Start UDP**.

---

## 5. Data export and TETRA Data window

The plugin separates two kinds of decoded information:

### 5.1 TCP feed (GPS positions only)

Every LIP location report is emitted as ONE JSON line, terminated by `\n`,
over a TCP connection to the Django map collector. The schema is exactly
the same as our other modules (AIS / ADS-B / APRS / DSD-FME /
radiosonde), so the same `listen_sdr` / `listen_*` management command
can consume the stream without changes:

```json
{"name":"SSI:1144322","mmsi":1144322,
 "date":"2026-06-01","time":"18:27:13",
 "lat":43.7034000,"lon":7.2663000,
 "type":"TETRA","speed":5,
 "info":"acc=20m dir=92deg"}
```

Field notes:
- `name` is a stable readable label (`SSI:<id>`).
- `mmsi` carries the same 24-bit SSI numerically, so the Django side can
  key contacts the same way as for AIS without schema changes.
- `speed` is in km/h, or `null` when the radio reports "unknown velocity".
- `info` is a free-text string; for TETRA it carries the position-accuracy
  estimate and direction-of-travel, with `unk` when those are unknown.

The connection is non-blocking: a background worker thread owns the socket,
reconnects with exponential backoff (1 s → 2 s → 4 s → 8 s → 16 s cap), and
drops the oldest queued line when the buffer overflows.

### 5.2 Floating "TETRA Data" window

Calls, SDS text messages and status codes are NOT sent on the TCP feed —
they live in an in-process floating window opened by the **"Show TETRA
Data window"** button at the bottom of the side menu. The window has
three tabs:

| Tab          | Content                                                  |
|--------------|----------------------------------------------------------|
| Calls        | Setup / connect / release / proceeding / tx-ceased rows |
| SDS Messages | Decoded text messages with src / dst / protocol / body  |
| Status       | Pre-coded status codes (ETSI vs operator-allocated)     |

Each tab keeps up to 500 entries newest-first, with a per-tab Clear
button. Hovering the window automatically suppresses the waterfall
scroll capture, so the mouse wheel inside the window does not retune
the VFO — same trick as the DSD-FME and POCSAG modules.

Event types emitted in Sessions 1-4:

```json
{"ts":"2026-06-01T14:23:11.482Z","type":"mle_pdu","pdisc":3,"pdut":1,"src":12345,"slot":1}
```

LIP transports supported:
- Protocol `0x0A` — LIP without SDS-TL (most common, direct payload)
- Protocol `0x89` — LIP with SDS-TL header (Transfer message type 1)
- Both Short Location Report (PDU type 0) and Long Location Report
  (PDU type 1) are decoded with the same accuracy.

Notes on internal SSI handling (visible in the floating window):
- 24-bit SSI. `0xFFFFFF` (16777215) means broadcast/all-call,
  often used as the destination of `D-STATUS` for group-wide codes.
- `call_id` is the 4-bit call identifier allocated by the SwMI; the same
  value appears on setup → connect → release for one call.
- Status codes `0x8000`–`0xFFFF` are ETSI standard (e.g. `0x8001` =
  emergency); `0x0001`–`0x7FFF` are operator-allocated ("10-codes").
- Release cause follows ETSI EN 300 392-2 clause 14.8.13.
- SDS text is always normalised to UTF-8 regardless of the on-air alphabet
  (GSM 7-bit packed, ISO-8859-1, or UCS-2/UTF-16BE).

---

## 6. About the bundled ETSI codec

The ETSI TETRA speech codec (`en_30039502v010301p0.zip`) is freely
available from the ETSI website but its redistribution licence is
unclear. This package includes it under `src/decoder/codec/` with the
five upstream patches already applied (makefile-cleanups, fix_64bit,
round_private, filename-case, log_stderr). The original ZIP md5sum is
`a8115fe68ef8f8cc466f4192572a1e3e`.

If you ever need to re-extract the codec from scratch, the
`download_and_patch.sh` script under `src/decoder/etsi_codec-patches/`
is still functional and will fetch the codec from ETSI.

---

## 7. Acknowledgements

Thanks to the osmo-tetra authors for their decoder, to AlexandreRouma
for SDR++, to cropinghigh for the original sdrpp-tetra-demodulator
module, and to dbdexter-dev whose `sdrpp_radiosonde` `CMakeLists.txt`
is the template followed by the in-tree mode here.

GPL-3.0 — same licence as upstream. The ETSI codec is under its own
licence terms.
