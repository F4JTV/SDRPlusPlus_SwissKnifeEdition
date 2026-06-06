# rtl_433 bridge module for SDR++

This module gives SDR++ access to the **complete rtl_433 decoder set (~320
protocols)** by linking the real [rtl_433](https://github.com/merbanan/rtl_433)
library and feeding it IQ from an SDR++ VFO — no decoders are re-implemented.

```
SDR++ VFO (complex float IQ, 250 kHz)
   -> convert to CU8  (rtl_433's native sample format)
   -> rtl_433 internal pipeline (replica of sdr_callback):
        magnitude_est_cu8 -> baseband_demod_FM -> pulse_detect_package
        -> run_ook_demods / run_fsk_demods   (ALL rtl_433 decoders)
   -> custom data_output_t  -> live message table in the SDR++ menu
```

Tested end-to-end: a synthetic Nexus CU8 frame fed through the wrapper is
decoded by the genuine rtl_433 `Nexus-TH` decoder with exact field values, and
`register_all_protocols` loads 283 (default) / 314 (incl. disabled) protocols.

## Why this vs the from-scratch `rtl_433_decoder` module?

| | `rtl_433_decoder` (port) | `rtl_433_bridge` (this) |
|---|---|---|
| Protocols | ~11 hand-ported | ~320 (the real thing) |
| Dependency | none | links librtl_433 |
| Maintenance | port each decoder | track rtl_433 releases |
| Best for | minimal/standalone builds | full coverage |

## Build (Ubuntu 24.04)

### 1. Build the rtl_433 static library (with -fPIC, no SDR backends)

The library must be compiled as position-independent code (`-fPIC`) because it
gets linked into a shared object (the SDR++ module `.so`). The SDR hardware
backends are disabled — the bridge feeds samples itself.

```bash
git clone https://github.com/merbanan/rtl_433.git
cd rtl_433 && mkdir -p build && cd build
cmake -DENABLE_RTLSDR=OFF -DENABLE_SOAPYSDR=OFF -DENABLE_OPENSSL=OFF \
      -DBUILD_TESTING=OFF \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DCMAKE_C_FLAGS="-fPIC" ..
make r_433 -j$(nproc)        # produces build/src/libr_433.a (PIC)
```

Verify:

```bash
ls -la src/libr_433.a        # must exist
```

> If you skip `-fPIC` the SDR++ link fails with
> `relocation R_X86_64_PC32 against symbol 'stderr' ... recompile with -fPIC`.

### 2. Drop the module into SDR++ and patch the root CMake

```bash
cd SDRPlusPlus
tar xzf rtl_433_bridge_sdrpp.tar.gz -C decoder_modules/
git apply root_CMakeLists.patch      # or edit CMakeLists.txt by hand (see patch)
```

The patch adds, once each, an option and an `add_subdirectory` block:

```cmake
option(OPT_BUILD_RTL_433_BRIDGE "Build the rtl_433 bridge module ..." OFF)
...
if (OPT_BUILD_RTL_433_BRIDGE)
add_subdirectory("decoder_modules/rtl_433_bridge")
endif (OPT_BUILD_RTL_433_BRIDGE)
```

> If you previously added the bridge manually, make sure these blocks appear
> **only once** — a duplicate `add_subdirectory` causes
> `binary directory ... is already used to build a source directory`.

### 3. Build SDR++ with the bridge enabled

Point `RTL_433_ROOT` at the **absolute path** of the rtl_433 tree whose
`build/src/libr_433.a` you built in step 1.

```bash
mkdir -p build && cd build
rm -f CMakeCache.txt                 # clean cache if reconfiguring
cmake .. -DOPT_BUILD_RTL_433_BRIDGE=ON -DRTL_433_ROOT=/home/$USER/Programmes/rtl_433
make rtl_433_bridge -j$(nproc)       # or: make -j$(nproc) to build everything
sudo make install
```

Alternatively, if you have a system `librtl_433` exposing pkg-config
`librtl_433`, omit `RTL_433_ROOT` and the module finds it automatically.

## Usage

Open **Module Manager**, add an `rtl_433_bridge` instance, tune to an ISM band
(433.92 / 868 / 315 / 915 MHz), drop the 250 kHz VFO on the signal.

The module menu holds the controls:

- **Gain** — pre-detector gain; raise it for weak SDRs.
- **Min level (dB)** / **SNR (dB)** — map to rtl_433's `-Y minlevel` / `-Y minsnr`.
- **Messages decoded** — running count.
- **Show / Hide messages** — toggles a **separate floating window** listing
  every decoded message in a `Time | Model | Data` table.

The messages window has:

- a **Clear** button (empties the table and resets the counter),
- an **Auto-scroll** checkbox — when enabled, the view follows new messages as
  long as you're already at the bottom; scroll up to inspect history and it
  stops following until you return to the bottom,
- a live "(N shown)" count.

The window is a normal SDR++/ImGui window: drag it anywhere, resize it, or dock
it. Columns are resizable.

## How it works (for maintainers)

`src/rtl433_lib.{h,cpp}` is a C++ wrapper over the rtl_433 C API:

- `r_create_cfg()` builds an `r_cfg` with a `dm_state` demod context.
- `register_all_protocols()` loads every device decoder.
- A custom `data_output_t` (`CallbackOutput`) is pushed onto
  `cfg->output_handler`; its `output_print` walks the decoded `data_t` linked
  list and converts each field into our `Message`.
- `feedCU8()` is a faithful replica of the core of rtl_433.c's `sdr_callback`:
  envelope (magnitude) → low-pass → FM discriminator → `pulse_detect_package`
  loop → `run_ook_demods` / `run_fsk_demods`.

`src/rtl433_bridge_dsp.h` converts SDR++ complex float IQ (≈[-1,1]) to CU8
(unsigned 8-bit, biased at 127.5, clamped at the rails) and calls `feedCU8`.

`src/main.cpp` is the SDR++ `ModuleManager::Instance`: VFO, DSP wiring, and the
ImGui live table.

### Notes / limitations

- The wrapper uses rtl_433's `r_private.h` (the `dm_state` struct). That header
  is internal; if a future rtl_433 release changes `dm_state`, the wrapper may
  need a small update. The public `r_api.h` functions it calls are stable.
- Fixed 250 kHz working bandwidth. For wideband FSK meters (M-Bus 100 kbps,
  some water meters) widen `RTL433_SAMPLERATE` in `main.cpp` and ensure the SDR
  source provides enough bandwidth.
- Per-protocol enable/disable from the UI is stubbed; rtl_433 has no clean
  runtime per-instance toggle once registered. To restrict protocols, register
  selectively (call `register_protocol` for chosen devices instead of
  `register_all_protocols`).
- `librtl_433` is GPLv2+; this module and SDR++ are GPLv3. The combined work is
  distributable under GPLv3.

## License

GPLv3 or later. Links rtl_433 (GPLv2+, © Tommy Vestermark, Benjamin Larsson,
Christian W. Zuckschwerdt and contributors); built on the SDR++ module API
(GPLv3, © Alexandre Rouma).
