# dsd_decoder — SDR++ module bridging DSD-FME

In-tree decoder module for SDR++ that bridges to **DSD-FME**
(<https://github.com/lwvmobile/dsd-fme>) as a subprocess for digital voice /
data decoding: DMR, P25 phase 1 & 2, NXDN48/96, dPMR, YSF, ProVoice,
EDACS Std/Net & EA, X2-TDMA and M17.

Target: Ubuntu 24 / Linux. `subprocess.h` is POSIX only — a Windows port
needs `CreateProcess` + anonymous pipes.

Version: **0.6.0**

## Architecture

```
                                          ┌──────────────────────────┐
   VFO ──► FM demod (48 kHz mono)         │     DSD-FME process      │
       audioHandler ─► PCM ring ─pipe(stdin)─► -i -  (s16le 48 kHz)  │
                                          │                          │
   console / errors ◄──── stdout/stderr ──┤  (line parser, ANSI       │
                                          │   strip, color)           │
                                          │                          │
   LRRP / GPS table  ◄──── tail file ────┤  -L <folder>/dsd_lrrp_X   │
                                          │                          │
                                          │  -1 -o udp:127.0.0.1:P   │
   outAudio stream ◄──── UDP recv ────────┤  (s16le mono 8 kHz)      │
   (registered ONCE with sigpath::         └──────────────────────────┘
    sinkManager — 48 kHz stereo)
```

The decoded audio is routed back into SDR++'s **sink manager**, exactly like
the radio and m17_decoder modules: it shows up under the regular audio sink
configuration (PulseAudio device pick, JACK, recorder, etc.). DSD-FME itself
no longer opens a PulseAudio device — its audio goes only over UDP to us.

## What changed in v0.6

* **Embedded RIGCTL server + CC/VC tracking.** The module now hosts its own
  small RIGCTL TCP server (cross-platform via SDR++'s `net::Listener`), so
  the external `rigctl_server` module is no longer required. Because we
  *receive* every `F <freq>` command from dsd-fme, we also know our exact
  state at every moment:
  * **CC (Control Channel)** — parked on the trunking CC, waiting for grants
    (green banner with the CC frequency)
  * **VC (Voice Channel)** — currently following a voice grant (orange
    banner with VC freq, TG, SRC, and elapsed time)
  
  The Trunking panel got a big colored status banner, a manual "Park on CC"
  override, a "Capture current freq as CC" button, an editable CC
  frequency, and a recent-grants mini-table (time, freq, duration, TG, SRC).
  An auto-return-to-CC timer (default 3 s of voice silence) handles systems
  that go quiet at end-of-transmission without emitting an explicit RIGCTL
  release.

## What changed in v0.5
## What changed in v0.5

* **LRRP positions to the Django map (TCP).** A new "Map server (LRRP)"
  section sends every positioned LRRP/GPS report as a single JSON line
  (`type:"lrrp"`) to the ADRASEC map collector, exactly like the AIS /
  ADS-B / APRS modules (same one-line schema, same default port 10100,
  same non-blocking client with auto-reconnect). Only reports that carry a
  latitude and longitude are sent. See `DJANGO_LRRP_INTEGRATION.md`.
* **Snap 12.5 kHz** added to the snap-interval combobox (v0.4.5).

## What changed in v0.4

* **Call history tab.** A new "Calls" tab in the detached window shows a
  table of decoded **voice** call sessions parsed from dsd-fme's console
  output: start time, duration, protocol, slot (1/2 for DMR), TG, source
  RID, Color Code or NAC, and encryption flag. Active calls are
  highlighted green; encrypted calls show a red "ENC" badge. The toolbar
  gains a "Save calls CSV" button that exports the whole table.
  Call sessions are grouped per-slot with a 2-second hangtime, so two
  simultaneous DMR talk groups on slots 1 and 2 are tracked as separate
  rows.
* **Voice-only filter (v0.4.1).** Data exchanges (SMS / Short Data, LRRP /
  GPS pings, DMRA, ARS, PDUs, data channel grants TD_GRANT/PD_GRANT, etc.)
  are excluded from the call history — only actual voice activity and
  voice channel grants (TV_GRANT/PV_GRANT, VC frames, P25 LDU1/LDU2) show
  up. LRRP/GPS data still appears in its dedicated tab.
* **Duplex / repeater voice detection (v0.4.4).** DMR repeater (Tier II,
  Capacity Plus, etc.) voice frames are printed by dsd-fme as
  `VLC SLOT n TGT=... SRC=... FLCO=...` lines that often lack a trailing
  "Group Call" / "Private Call" suffix, so the v0.4.x voice filter was
  dropping them and the Calls tab stayed empty in duplex mode. The filter
  now also recognises the DMR voice-burst markers VLC / VC* / TLC, plus a
  fallback that treats any `SLOT n ... TGT=` line (that is not already a
  data/PDU/header line) as a voice frame. DMO behaviour is unchanged.
* **Console cleanup (v0.4.3).** dsd-fme's startup banner uses Unicode
  box-drawing characters (▓▒░ etc.) that the default ImGui font cannot
  render — they showed up as a screenful of `?` at the top of the
  console window. The line stripper now drops any non-printable / non-
  ASCII byte (tabs preserved) and skips lines that become whitespace-
  only after stripping. Normal decoded output is unaffected.
* **Parser fixes (v0.4.2)** for the DMR DMO format emitted by recent
  dsd-fme builds (e.g. `SLOT 1 TGT=20806 SRC=2081371 Group Call`):
  * `TGT=` is now recognised as a talkgroup alias (previously only `TG=`,
    `TGID=`, `Talkgroup`, `Target` were matched);
  * the slot regex is case-insensitive, so uppercase `SLOT 1` is captured
    correctly;
  * Color Code converges to the latest sync-line value during a call —
    dsd-fme often emits `Color Code=15` (the 4-bit "unknown" placeholder)
    during sync acquisition before locking onto the real CC. The newer
    value from a subsequent Sync line is now propagated to any open call.

## What changed in v0.3

* **Trunking support.** A new "Trunking" section in the menu wires dsd-fme's
  RIGCTL client to SDR++'s `rigctl_server` module. When enabled, dsd-fme
  takes control of the source frequency to follow channel grants on a P25 /
  DMR / NXDN / EDACS control channel, or scan a list (`-Y`).
  See the "Trunking" section below.

## What changed in v0.2

* **Removed the per-module "Audio out" combobox.** The output stream is
  registered with `sigpath::sinkManager` and the user picks the audio
  device once, globally, like for every other audio-emitting module.
* **Snap-interval combo** (100 Hz, 1 kHz, 2.5 kHz, 5 kHz, 10 kHz, 25 kHz),
  applied live without restarting the decoder.
* **LRRP folder picker** using the standard `FolderSelect` widget, same as
  the POCSAG module. The actual file is `<folder>/dsd_lrrp_<instance>.txt`.
* **Crash on disable/enable cycle fixed.** Root cause was a double VFO:
  the constructor created one and `enable()` created a second one without
  deleting the first, so the next `createVFO` with the same name blew up.
  Lifecycle is now the same convention as the radio / POCSAG modules:
  `enabled = true` by default, the constructor calls the same setup path
  as `enable()`, and `fm`/`audioSink` are `std::unique_ptr<>` so they are
  rebuilt from scratch on every cycle instead of re-`init()`-ed.

## Build (Ubuntu 24)

```bash
sudo apt install build-essential cmake git pkg-config \
                 libfftw3-dev libglfw3-dev libvolk2-dev \
                 libsoapysdr-dev libairspyhf-dev libhackrf-dev \
                 librtaudio-dev libxkbcommon-dev libxcb-image0-dev \
                 libxcb-util1 libxcb-cursor-dev libxcb-randr0-dev \
                 libxcb-keysyms1-dev

# install dsd-fme itself, the binary needs to be on PATH (or set via the
# "dsd-fme path" field in the module's "Advanced" section).
sudo apt install dsd-fme  # or build from source: https://github.com/lwvmobile/dsd-fme

git clone https://github.com/AlexandreRouma/SDRPlusPlus.git
cd SDRPlusPlus

# drop the module folder in place
cp -r /path/to/dsd_decoder decoder_modules/

# edit the root CMakeLists.txt (see below), then:
mkdir build && cd build
cmake .. -DOPT_BUILD_DSD_DECODER=ON
make -j$(nproc)
sudo make install
```

### Root `CMakeLists.txt` edits

In the first `# Decoders` block (around line 55), add:

```cmake
option(OPT_BUILD_DSD_DECODER "Build the DSD-FME digital voice/data decoder module (needs the dsd-fme binary at runtime)" OFF)
```

In the second `# Decoders` block (around line 291), add:

```cmake
if (OPT_BUILD_DSD_DECODER)
    add_subdirectory("decoder_modules/dsd_decoder")
endif (OPT_BUILD_DSD_DECODER)
```

## Use

1. Add a "DSD Decoder" module instance from the module manager.
2. Drop the VFO on a digital voice / data carrier (DMR repeater, P25 control
   channel, etc.).
3. Pick the mode in the menu (or leave "Auto" for DMR / P25 detection).
4. Decoded audio comes out the **global sink** (Audio menu in SDR++).
5. "Open DSD-FME console" opens a detached window with the dsd-fme stdout
   (with ANSI stripped + color coding) and an LRRP / GPS table.

## Notes

* The audio path is FM-discriminated wideband audio (no de-emphasis), 48 kHz
  mono s16le, fed to dsd-fme on its stdin (`-i -`). Adjust "Bandwidth" so
  the digital signal sits inside the VFO; 12.5 kHz is sane for narrow
  modes, 25 kHz for wide. "Input gain" trims the level into dsd-fme; pin
  the dsd-fme console to watch its symbol histogram if you tune it.
* The LRRP file is appended-to by dsd-fme via `-L`. The module tails it
  and parses lat/lon/source out of each line.
* Encryption keys can be passed as hex via the "Key (-H hex)" field
  (RC4/DES/AES); ENC voice without a key stays muted by dsd-fme itself.
* "Extra args" is split on whitespace and appended to the dsd-fme command
  line for anything not exposed in the UI.

## Trunking

To follow a trunked system (P25, DMR Tier III, Cap+/Con+, EDACS, NXDN trunked):

1. **Load the `rigctl_server` module in SDR++** (Module Manager → Add → pick
   "rigctl_server"). Open its menu and:
   * set the TCP port (default 4532),
   * bind it to **this** decoder's VFO (the "Controlled VFO" dropdown),
   * leave it set to "Tune mode: VFO" so that frequency changes move the
     source carrier rather than just the VFO offset.
   * Click Start; the status should read "Listening".

2. **Park the SDR on the control channel.** Drop the dsd_decoder VFO at
   offset 0 on the CC frequency.

3. In the dsd_decoder menu, expand "Trunking":
   * Pick `-T` (trunking, CC-tracking) **or** `-Y` (scan a static list).
     They are usually mutually exclusive.
   * Set "RIGCTL port" to whatever the rigctl_server is bound to.
   * Click "Test RIGCTL" — it should turn green ("reachable").
   * For **P25 P1/P2**, the channel map is optional (dsd-fme learns the
     band plan from the Identifier Update TSBKs).
   * For **DMR Tier III / Cap+ / Con+ / XPT, NXDN trunked, EDACS**, point
     "Channel map (-C)" at a CSV mapping LCN → frequency (Hz). See
     `dsd-fme/examples/*.csv` for sample files.
   * "Groups (-G)" is an optional CSV mapping talkgroup IDs to labels.

4. Hit "Apply & (re)start". The console tab in the popup window shows the
   trunking decisions ("Voice Grant → tuning to xxx.yyy MHz", etc.).

### Channel map / groups file format

Minimal channel_map.csv (DMR Tier III, frequencies in Hz):

```csv
ChannelNumber(dec),frequency(Hz)
999,456318750
36,455756250
54,455981250
81,456318750
```

Minimal groups.csv:

```csv
TGID,Label,Priority
1001,Dispatch,1
1002,Tac1,2
```

The 999 row above is the "default CC" entry that DSD-FME tries first on
startup; with RIGCTL it can also just poll the VFO frequency to detect the
CC, in which case any non-existent first row is fine.

### Common pitfalls

* `rigctl_server` not started, or started on the wrong port → "Test RIGCTL"
  is red; dsd-fme can't retune.
* `rigctl_server` bound to a *different* VFO than the decoder → the source
  retunes but our audio path stays on the old frequency. Fix: in
  rigctl_server, pick the dsd_decoder VFO from the "Controlled VFO" combo.
* Source center frequency too far from the CC after a grant → the VFO
  offset would push the new voice channel outside the SDR's IF bandwidth.
  Use a wide enough IF (SDR source bandwidth) to cover the whole talk
  group span, or let rigctl_server use "Tune mode: Source" so each retune
  centers the SDR on the new channel.
* Encrypted voice (`ENC` shown) is muted by dsd-fme by default and the
  trunking logic skips ENC grants unless you toggle that behavior with
  the appropriate dsd-fme flag in "Extra args" (e.g. `-p`).
