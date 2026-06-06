# SDR++ — Extended Decoder & Telemetry Edition

A fork of [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus) (the cross-platform
SDR receiver by Alexandre Rouma) that adds a large suite of **decoder, demodulator
and telemetry modules** on top of the upstream core. Every upstream source, sink
and feature is preserved; this fork only *adds* modules and a few cross-cutting
behaviours.

The headline addition is a unified **telemetry pipeline**: many decoders can stream
their positioned results as newline-delimited JSON to a single TCP collector, where
a bundled **Django + Leaflet web map** (`sdr_map`) plots ships, aircraft, APRS
stations, radiosondes and trunked-radio GPS pings in real time.

Primary target: **Ubuntu 24.04 / Linux**. Most modules are portable; a few rely on
POSIX-only mechanisms (subprocess pipes, `welle.io`) and are Linux-first — these are
flagged per module below.

---

## Relationship to upstream

This fork tracks the upstream SDR++ architecture exactly: modules are independent
shared objects discovered at runtime, added through the **Module Manager**, and
built from the top-level `CMakeLists.txt` via `OPT_BUILD_*` switches. New modules
follow the upstream conventions (ImGui UI, `dsp` pipeline, `sigpath` integration),
so they coexist cleanly with stock sources, sinks and decoders.

All third-party decode engines are used as upstream projects (linked or driven as
subprocesses), never re-implemented:

| Engine | Used by | Integration |
|--------|---------|-------------|
| [dsd-fme](https://github.com/lwvmobile/dsd-fme) + [mbelib](https://github.com/szechyjs/mbelib) | `dsd_decoder` | child process (audio over pipe/UDP) |
| [dumpvdl2](https://github.com/szpajder/dumpvdl2) + [libacars](https://github.com/szpajder/libacars) | `vdl2_decoder` | child process (I/Q over pipe) |
| [rtl_433](https://github.com/merbanan/rtl_433) | `rtl_433_decoder` | linked library (`RTL_433_ROOT`) |
| [welle.io](https://github.com/AlbrechtL/welle.io) | `dab_decoder` | in-tree backend |
| [Dream](https://sourceforge.net/projects/drm/) | `drm_decoder` | in-tree backend |
| sondedump | `radiosonde_decoder` | in-tree backend |
| FLDIGI receiver code | `mfsk_decoder`, `psk_decoder`, `rtty_decoder` | faithful port |
| `dec406` (F4EHY) | `cospas_sarsat_decoder` | reference implementation |

---

## At a glance — modules added or reworked in this fork

| Module | Default | What it does | Extra deps |
|--------|:------:|--------------|-----------|
| `ais_decoder` | ON | Marine AIS ship tracking (161.975 / 162.025 MHz) + TCP map output | — |
| `adsb_decoder` | ON | ADS-B aircraft (1090 MHz, Mode S ES) + TCP map output | — |
| `acars_decoder` | ON | VHF ACARS (2400-baud MSK) message decode | — |
| `vdl2_decoder` | ON | VDL Mode 2, full stack, via `dumpvdl2` front-end | dumpvdl2, libacars |
| `aprs_decoder` | ON | APRS (AX.25 / AFSK 1200) + weather stations + TCP map output | — |
| `cospas_sarsat_decoder` | ON | 406 MHz SAR distress beacons (ELT / EPIRB / PLB / SSAS) | — |
| `dsd_decoder` | ON | Digital voice/data (DMR, P25 p1/p2, NXDN, dPMR, YSF, …) via DSD-FME | dsd-fme binary |
| `tetra_demodulator` | ON | TETRA downlink demod + decode (osmo-tetra backend, codec bundled) | — |
| `radiosonde_decoder` | ON | Weather sondes (RS41, DFM, M10/M20, iMS100, iMet-4, C50, MRZ-N1, Meisei) + TCP map output | — |
| `rtl_433_decoder` | ON | ~320 ISM-band sensor protocols via linked rtl_433 | rtl_433 lib |
| `sstv_decoder` | ON | Analog SSTV imaging, auto mode + slant correction | — |
| `wefax_decoder` | ON | HF weather fax (radiofax) | — |
| `drm_decoder` | ON | Digital Radio Mondiale audio (Dream backend, xHE-AAC) | fdk-aac, faad-drm |
| `drm_image_decoder` | ON | HamDRM digital images (DRM SSTV) | fftw3 |
| `dab_decoder` | ON | DAB / DAB+ broadcast audio (welle.io backend) — **Linux only** | — |
| `mfsk_decoder` | ON | FLDIGI MFSK family (MFSK16, …) | — |
| `rtty_decoder` | ON | Baudot/ITA2 RTTY (incl. DWD Pinneberg marine preset) | — |
| `psk_decoder` | ON | BPSK/QPSK (PSK31 family) | — |
| `ft8_decoder` | ON | FT8 / FT4 / WSPR | fftw3 |
| `gps_decoder` | ON | GPS L1 C/A acquisition + tracking (1575.42 MHz) | — |
| `sdr_map_launcher` | ON | Start/stop the companion Django map + open it in a browser | Python/Django |

Modules inherited unchanged from upstream (radio, pager, meteor, CW, recorder,
scanner, frequency manager, rigctl client/server, IQ exporter, Discord presence,
and all hardware sources/sinks) are not re-documented here.

---

## Cross-cutting design

These behaviours are shared by the new modules and are worth understanding once.

### 1. The telemetry pipeline → `sdr_map`

Position-aware decoders (AIS, ADS-B, APRS, radiosonde, and DSD-FME LRRP/GPS) can
emit **one JSON line per object** to a TCP collector. All of them share:

* the **same one-line JSON schema** with a `type` field (`"AIS"`, `"ADSB"`,
  `"APRS"` / `"APRS Meteo"`, `"radiosonde"`, `"lrrp"`, …);
* the **same default destination**, `127.0.0.1:10100` (a single port for every
  module);
* a **non-blocking TCP client** with automatic reconnect, so a missing or
  restarting collector never stalls the decoder;
* **TCP is opt-in and never auto-starts**: the `tcpEnabled` flag is deliberately
  *not* persisted to the SDR++ config, so a fresh launch never tries to connect
  until you tick the box again.

The collector is the bundled `sdr_map` Django project (see `sdr_map_launcher`),
which renders everything live on a Leaflet map.

### 2. Detached windows that don't fight the waterfall

Decoders with rich output (APRS, ACARS, DSD calls, contacts tables, image
previews) open **movable/resizable detached windows**. These windows are guarded
so that dragging them over the waterfall **does not retune the VFO** — a small but
important ergonomic fix for operational use.

### 3. In-tree *and* out-of-tree builds

Each added module ships a `CMakeLists.txt` that auto-detects whether it is being
built inside the SDR++ source tree or standalone (via the `sdrpp_module.cmake`
macro). No flags to pass either way.

### 4. Voice decoders route through the SDR++ sink manager

`dsd_decoder` (and the other audio-producing decoders) register their output with
SDR++'s **sink manager**, exactly like the stock `radio` module — so decoded audio
appears under the normal audio sink configuration (PulseAudio device selection,
JACK, recorder, etc.) instead of opening its own audio device.

---

## Building (Ubuntu 24.04)

### Quick path

A one-shot script (`install_sdrpp_custom.sh`) installs all dependencies, clones and
builds the external engines (mbelib, dsd-fme, rtl_433, libacars, dumpvdl2,
welle.io, Dream) and finally configures and builds SDR++ with every module enabled.
Run it with **bash** (it uses `set -euo pipefail`, which `dash`/`sh` do not
support):

```bash
chmod +x install_sdrpp_custom.sh
./install_sdrpp_custom.sh
```

### Manual outline

1. Install the build toolchain and SDR/audio/codec dependencies (see the script's
   `apt install` block for the exact list).
2. Build and install the external engines first, because SDR++ links or drives
   them:
   * **mbelib** then **dsd-fme** (`sudo make install` + `ldconfig`) for
     `dsd_decoder`;
   * **rtl_433** built as a static library only (target `r_433`, *not* installed)
     for `rtl_433_decoder` — its path is passed as `RTL_433_ROOT`;
   * **libacars** then **dumpvdl2** for `vdl2_decoder`;
   * **welle.io** cloned into `decoder_modules/dab_decoder/` and **Dream** into
     `decoder_modules/drm_decoder/` (in-tree backends).
3. Configure and build SDR++:

   ```bash
   cmake -S . -B build -DRTL_433_ROOT="$HOME/rtl_433"
   cmake --build build -j"$(nproc)"
   sudo cmake --install build
   ```

### Build options

All modules are toggled with `-DOPT_BUILD_<NAME>=ON|OFF`. See the
[CMake options reference](#cmake-options-reference) at the end.

> **DAB caveat.** `dab_decoder` is **Linux-only** (the welle.io backend) and is
> gated by `OPT_BUILD_DAB_DECODER`. Make sure the option resolves to `ON` (CMake
> caches option values, so a value already stored in `CMakeCache.txt` wins over the
> default in `CMakeLists.txt` — pass `-DOPT_BUILD_DAB_DECODER=ON` explicitly or use
> a fresh build directory if in doubt).

After installation, launch SDR++ and add each module through the **Module Manager**
(left menu → *Module Manager* → pick the module type → give it a name).

---

## Module reference

### Maritime, aeronautical & APRS tracking

#### `ais_decoder` — Marine AIS
GMSK demodulation at 9600 baud on the two VHF AIS channels (AIS 1 = 161.975 MHz,
AIS 2 = 162.025 MHz). Full chain: NRZI decode → HDLC flag sync → bit de-stuffing →
CRC-16/X.25 → field parsing for message types 1–5, 18, 19, 21, 24. A
movable/resizable **contacts window** lists each vessel (name / MMSI, position,
course, speed, type). Optional TCP output streams contacts as `type:"AIS"` JSON to
the map.

#### `adsb_decoder` — ADS-B (1090 MHz)
Mode S Extended Squitter, PPM at 1 Mbit/s, processed dump1090-style on a magnitude
stream at 2 MHz (2 samples/bit). Chain: preamble detection → PPM slicing → Downlink
Format → **CRC-24** (Mode S polynomial `0x1FFF409`) → ME field parsing
(identification, airborne/surface position, velocity). **CPR** position decoding,
both global (even/odd frame pair) and local. Each aircraft is forwarded as
`type:"ADSB"` JSON to the map, mirroring the AIS module.

#### `acars_decoder` — VHF ACARS
Self-contained **AM + MSK** demodulation at 2400 baud (nothing to wire on the audio
side). Decodes the full ACARS frame (mode, registration, ACK/NAK, label, block id,
text), with odd-parity + CCITT CRC error checking. Messages are shown in a detached
window. Built on `libacars` for higher-level application parsing.

#### `vdl2_decoder` — VDL Mode 2 (dumpvdl2 front-end)
Channelises a VDL2 channel to 105 kHz complex baseband and pipes it to a
**`dumpvdl2`** child process as 16-bit I/Q, then displays dumpvdl2's decoded text in
a detachable scrolling log. Because the heavy lifting is done by dumpvdl2, you get
its **complete** decode chain: ACARS over AVLC plus the higher layers — X.25 /
ISO 8208, CLNP, and the ATN applications (CPDLC controller–pilot clearances, ADS-C
position/telemetry, Context Management) and MIAM — all integrated with the SDR++
waterfall, VFO and multi-channel UI.

#### `aprs_decoder` — APRS + weather
AX.25 / AFSK 1200 (Bell 202). Real-time decode of positions, MIC-E, objects and
items, plus **APRS weather stations** (temperature, humidity, wind, gusts,
pressure, rain) for both position+weather (`_` symbol) and positionless reports. A
**two-tab detached window** separates *Positions* from *Weather* (metric units) and
does not move the VFO when dragged. TCP output sends one JSON line per positioned
object: `type:"APRS"` for positions and `type:"APRS Meteo"` for weather, in the same
base format as the AIS module.

### Search & rescue

#### `cospas_sarsat_decoder` — 406 MHz beacons
Real-time decoding of Cospas-Sarsat 406 MHz emergency distress beacons (ELT, EPIRB,
PLB, SSAS), based on the `dec406` decoder by **F4EHY**. BPSK demodulation and frame
parsing of the beacon message, surfacing the encoded identity and (where present)
position. In-tree build for Ubuntu 24.04.

### Digital voice & data

#### `dsd_decoder` — DSD-FME bridge
Bridges to **DSD-FME** as a subprocess for digital voice/data: DMR, P25 phase 1 & 2,
NXDN48/96, dPMR, YSF, ProVoice, EDACS (Std/Net & EA), X2-TDMA and M17. FM-demodulated
48 kHz mono PCM is piped to DSD-FME's stdin; decoded audio comes back over UDP and is
registered **once** with the SDR++ sink manager (so DSD-FME no longer opens its own
PulseAudio device). Highlights:

* **Embedded RIGCTL server + CC/VC tracking.** The module hosts its own small RIGCTL
  TCP server (cross-platform via SDR++'s `net::Listener`), so the external
  `rigctl_server` is not required. Because it receives every `F <freq>` command from
  DSD-FME, it always knows whether it is parked on the **Control Channel** (green
  banner) or following a **Voice Channel** grant (orange banner with VC freq, TG,
  SRC, elapsed time). Includes a manual "Park on CC" override, "Capture current freq
  as CC", an editable CC frequency, a recent-grants mini-table, and an
  auto-return-to-CC timer for systems that go silent without an explicit release.
* **Call history tab.** A *Calls* table parsed from DSD-FME's console output: start
  time, duration, protocol, slot (1/2 for DMR), TG, source RID, Color Code / NAC,
  encryption flag. Active calls are green; encrypted calls show a red **ENC** badge.
  Per-slot grouping with a 2 s hangtime tracks two simultaneous DMR talkgroups as
  separate rows. CSV export included.
* **Voice-only filter.** Data exchanges (SMS/Short Data, LRRP/GPS, DMRA, ARS, PDUs,
  data grants) are excluded from the call history — only real voice activity and
  voice grants appear.
* **LRRP/GPS to the map.** Positioned LRRP reports are sent as `type:"lrrp"` JSON to
  the same map collector (port 10100), with the same non-blocking auto-reconnect
  client as AIS/ADS-B/APRS. See `DJANGO_LRRP_INTEGRATION.md`.

> Linux/POSIX target: the subprocess plumbing is POSIX-only; a Windows port would
> need `CreateProcess` + anonymous pipes.

#### `tetra_demodulator` — TETRA downlink
Demodulates and decodes TETRA downlink. Signal chain:
`VFO → AGC → FLL → RRC → ML timing recovery → Costas → constellation → symbol
extractor → differential decoder → bit unpacker → osmo-tetra decoder → sink`. The UI
is sectioned with collapsing headers, LED indicators, status pills, and a framed
timeslot grid with **per-slot encryption badges** and large frame counters. The ETSI
TETRA speech codec is **pre-extracted and patched** in-tree, so no
`download_and_patch.sh` step is needed — just configure and build.

### Telemetry & sensors

#### `radiosonde_decoder` — Weather sondes
In-tree sondedump backend supporting **RS41, DFM (06/09), M10/M20, iMS100, iMet-4,
C50, MRZ-N1 and Meisei**. Decodes PTU (pressure/temperature/humidity) and GPS, with
optional **TCP output** (`type:"radiosonde"`) to the map and the same non-blocking
client as the tracking modules.

#### `rtl_433_decoder` — ISM 433/868 MHz sensors
Gives SDR++ the **complete rtl_433 decoder set (~320 protocols)** by linking the
real rtl_433 library and feeding it I/Q from a VFO — no decoders are
re-implemented. Internally: SDR++ complex-float I/Q (250 kHz) → convert to CU8 →
rtl_433's native pipeline (`magnitude_est_cu8 → baseband_demod_FM →
pulse_detect_package → run_ook_demods / run_fsk_demods`) → a custom `data_output_t`
that feeds a live message table in the SDR++ menu. Requires `RTL_433_ROOT` at
configure time.

### Imaging

#### `sstv_decoder` — Analog SSTV
Live SSTV image decode with automatic mode detection, **RANSAC-based slant
correction**, and live preview. Supports the common families (Robot 36/72, Scottie,
Martin, PD 50/90/120/160/180/240/290, …). The source compiles against multiple SDR++
versions thanks to compile-time detection of the `dsp::demod::FM<float>::init()`
signature (both the 4-arg and 5-arg variants are handled transparently).

#### `wefax_decoder` — HF weather fax
Decodes HF radiofax (WEFAX) transmissions into images directly inside SDR++.

#### `drm_image_decoder` — HamDRM images (DRM SSTV)
Decodes HamDRM digital still images (DRM-based SSTV). Depends on fftw3.

### Digital audio broadcast

#### `drm_decoder` — Digital Radio Mondiale
DRM broadcast audio using a Dream-based in-tree backend. xHE-AAC is enabled via
**libfdk-aac**; classic DRM-AAC via a DRM-capable libfaad (`libfaad_drm`, detected at
configure time).

#### `dab_decoder` — DAB / DAB+ (Linux only)
DAB and DAB+ broadcast audio using the **welle.io** backend cloned in-tree. Gated by
`OPT_BUILD_DAB_DECODER`; Linux-only.

### HF text & data (FLDIGI family)

These three share a common UI template: sideband selector (USB / LSB / NFM),
interactive AF band display (click/drag to set the AF), AFC, AF slider and power
squelch.

#### `mfsk_decoder` — MFSK
A **faithful port of the FLDIGI MFSK receiver** (Viterbi and interleaver classes
taken verbatim, identical IZ8BLY varicode, same receive math), validated against a
real off-air MFSK16 recording. Includes a Goertzel band-spectrum widget, rolling AFC,
quality-gated re-acquisition and replay-on-lock.

#### `rtty_decoder` — Baudot/ITA2 RTTY
Two-tone FSK (mark/space) demodulation via sliding correlators, full Baudot/ITA2
decode (LTRS/FIGS shift, optional USOS), and **8 FLDIGI presets** including
**50/450 (DWD Pinneberg marine weather)**. Interactive AF band display with a shaded
band at the signal width (shift + baud), AFC locking on the balanced mark/space pair,
reverse and squelch.

#### `psk_decoder` — PSK
BPSK/QPSK (PSK31 family) decode, the UI template the RTTY and MFSK modules are
modelled on.

### Navigation

#### `gps_decoder` — GPS L1 C/A
Acquisition and tracking of GPS L1 C/A. The VFO is centred on **1575.42 MHz** with a
2.0 MHz bandwidth; a panel lists acquired satellites and their tracking state.

### Companion

#### `sdr_map_launcher` + `sdr_map` (Django map)
A panel that starts/stops the bundled `sdr_map` Django web server and opens the live
Leaflet map in your browser, all from inside SDR++. The panel exposes: project dir,
Python interpreter, web host/port (default `8000`), and the **single TCP collector
host/port (default `0.0.0.0:10100`)** that every telemetry module targets. While the
server runs, the fields lock (changing them wouldn't affect the running child).
Decoded objects (AIS, ADS-B, APRS, radiosonde, LRRP) arrive on the TCP port and are
plotted live with SVG icons, flags, light/dark palettes and service-worker caching.

---

## CMake options reference

Custom / notable switches (all `-DOPT_BUILD_<NAME>=ON|OFF`):

| Option | Default | Notes |
|--------|:------:|-------|
| `OPT_BUILD_AIS_DECODER` | ON | AIS + TCP output |
| `OPT_BUILD_ADSB_DECODER` | ON | ADS-B + TCP output |
| `OPT_BUILD_ACARS_DECODER` | ON | VHF ACARS |
| `OPT_BUILD_VDL2_DECODER` | ON | needs the `dumpvdl2` binary at runtime |
| `OPT_BUILD_APRS_DECODER` | ON | APRS + weather + TCP output |
| `OPT_BUILD_COSPAS_SARSAT_DECODER` | ON | 406 MHz beacons |
| `OPT_BUILD_DSD_DECODER` | ON | needs the `dsd-fme` binary at runtime |
| `OPT_BUILD_TETRA_DEMODULATOR` | ON | codec pre-bundled |
| `OPT_BUILD_RADIOSONDE_DECODER` | ON | + TCP output |
| `OPT_BUILD_RTL_433_BRIDGE` | ON | links librtl_433; needs `RTL_433_ROOT` |
| `OPT_BUILD_SSTV_DECODER` | ON | |
| `OPT_BUILD_WEFAX_DECODER` | ON | |
| `OPT_BUILD_DRM_DECODER` | ON | fdk-aac / faad-drm |
| `OPT_BUILD_DRM_IMAGE_DECODER` | ON | fftw3 |
| `OPT_BUILD_DAB_DECODER` | ON | **Linux only** (welle.io) |
| `OPT_BUILD_MFSK_DECODER` | ON | |
| `OPT_BUILD_RTTY_DECODER` | ON | incl. DWD Pinneberg preset |
| `OPT_BUILD_PSK_DECODER` | ON | |
| `OPT_BUILD_FT8_DECODER` | ON | FT8/FT4/WSPR, fftw3 |
| `OPT_BUILD_GPS_DECODER` | ON | GPS L1 C/A |
| `OPT_BUILD_SDR_MAP_LAUNCHER` | ON | companion Django map |

`RTL_433_ROOT` must point at a built rtl_433 source tree (the `r_433` static
library target), e.g. `-DRTL_433_ROOT="$HOME/rtl_433"`.

---

## Notes & caveats

* **Linux-first modules:** `dsd_decoder` and `vdl2_decoder` rely on POSIX subprocess
  pipes; `dab_decoder` uses the welle.io backend. These are Linux-targeted in this
  fork.
* **Runtime binaries:** `dsd_decoder` needs `dsd-fme` on `PATH`; `vdl2_decoder` needs
  `dumpvdl2`. Both are installed by the build script.
* **TCP never auto-connects:** telemetry output stays off until you enable it in the
  module — the enabled flag is intentionally not saved to config.
* **CMake option caching:** an `OPT_BUILD_*` value already stored in
  `CMakeCache.txt` overrides the default in `CMakeLists.txt`. Use an explicit `-D…`
  or a clean build directory when changing what gets built.

---

## Credits & licensing

This is a fork of **SDR++** by **Alexandre Rouma** — see the upstream project for
the core licence and authorship. The added modules build on the work of:

* **dsd-fme** (lwvmobile) and **mbelib** — digital voice/data decoding;
* **dumpvdl2** and **libacars** (Tomasz Lemiech / szpajder) — VDL2 / ACARS;
* **rtl_433** (Benjamin Larsson / merbanan and contributors) — ISM decoders;
* **welle.io** — DAB/DAB+ backend;
* **Dream** — DRM backend;
* **FLDIGI** — the MFSK/PSK/RTTY receiver code the HF-text modules are ported from;
* **dec406** (F4EHY) — the Cospas-Sarsat reference decoder.

Each module and bundled engine retains its own upstream licence. Refer to the
respective project directories and `LICENSE` files for terms.
