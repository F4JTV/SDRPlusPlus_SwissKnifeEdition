# VDL Mode 2 Decoder for SDR++ (dumpvdl2 front-end)

An SDR++ plugin that brings full **VDL Mode 2** (VHF Data Link Mode 2) decoding
into the SDR++ UI by driving [dumpvdl2](https://github.com/szpajder/dumpvdl2) as
a child process. The module channelises a VDL2 channel to 105 kHz complex
baseband, pipes it to dumpvdl2 as 16-bit I/Q, and displays dumpvdl2's decoded
text output in a detachable, scrolling log window.

Because the work is done by dumpvdl2, you get its **complete** decode chain:
ACARS over AVLC, plus the higher network/application layers — X.25/ISO 8208,
CLNP, and the ATN applications (CPDLC controller-pilot clearances, ADS-C
position/telemetry reports, Context Management), as well as MIAM — all rendered
exactly as dumpvdl2 formats them, but integrated with the SDR++ waterfall, VFO
and multi-channel UI.

> This module is a **front-end**: it does not decode anything itself. **dumpvdl2
> must be installed** and reachable (on `PATH` or via the configured path).

## What is VDL Mode 2?

VDL2 is a 31.5 kbit/s digital link in the 136 MHz aeronautical band: differential
8-PSK, 10 500 symbols/s (3 bits/symbol), Reed–Solomon RS(255,249), AVLC link
layer. The Common Signalling Channel is 136.975 MHz; other channels in common use
include 136.700/.725/.775/.800/.825/.875/.925 MHz (regional allocations vary).

## Requirements

- **Linux** (the front-end uses POSIX `fork`/`exec`/`pipe`; Windows is not
  supported in this mode).
- **dumpvdl2**, built and installed. It is not packaged in the Ubuntu
  repositories, so build it from source:

  ```sh
  # dumpvdl2 needs libacars first
  sudo apt install build-essential cmake pkg-config libglib2.0-dev zlib1g-dev libxml2-dev
  git clone https://github.com/szpajder/libacars
  cd libacars && mkdir build && cd build && cmake .. && make && sudo make install && sudo ldconfig
  cd ../..
  git clone https://github.com/szpajder/dumpvdl2
  cd dumpvdl2 && mkdir build && cd build && cmake .. && make && sudo make install
  dumpvdl2 --version          # verify it is on PATH
  ```

## Building the module (Ubuntu 24.04)

Place this folder at `decoder_modules/vdl2_decoder/` in an
[SDR++](https://github.com/AlexandreRouma/SDRPlusPlus) checkout (the included
`apply_to_sdrpp.sh` does this and wires up the build), then:

```sh
cd SDRPlusPlus
mkdir -p build && cd build
cmake .. -DOPT_BUILD_VDL2_DECODER=ON
make -j$(nproc)
sudo make install
```

The module itself has no external library dependencies (it only links pthreads);
dumpvdl2 is a *runtime* dependency, invoked as a separate process.

### Automated integration

```sh
./apply_to_sdrpp.sh /path/to/SDRPlusPlus
```

Idempotent; it copies the module in, adds the `OPT_BUILD_VDL2_DECODER` option and
`add_subdirectory(...)` to the root `CMakeLists.txt`, and registers
`vdl2_decoder.so` in the default module list in `core/src/core.cpp`.

## Usage

1. Start SDR++ and your SDR device.
2. Enable the **vdl2_decoder** module if needed.
3. If `dumpvdl2` is not on your `PATH`, type its full path in the **dumpvdl2**
   field.
4. Pick a **Channel** (defaults to 136.975 MHz CSC). This tunes the radio and
   restarts dumpvdl2 for the new frequency. The VFO is locked to 14 kHz and
   produces the 105 kHz baseband dumpvdl2 expects.
5. Watch **Status** — it should read `running`. If it shows
   `dumpvdl2 not found`, fix the path. Use **Start/Restart** as needed.
6. Click **Show Messages** to open the log window. Decoded messages appear as
   aircraft transmit, formatted by dumpvdl2 (one block per message).
7. Use the **Filter** row at the top of the window to show or hide message types
   (ACARS, X.25, CLNP, CPDLC, ADS-C, AVLC control frames, Other). Each toggle
   shows a live count, and **All**/**None** select or clear everything at once.
8. Optionally tick **Log to file** and pick a folder to append the decoded text
   to `vdl2_dumpvdl2.log` (the file always receives every message, regardless of
   the on-screen filter).

## How it works

The module creates a VFO whose output sample rate is exactly
`10 500 × 10 = 105 000 Hz` — dumpvdl2's internal rate at `--oversample 1` — so no
resampling is required. Each VFO block of complex floats is converted to
interleaved signed 16-bit I/Q (matching dumpvdl2's `S16_LE` input, which divides
by 32768) and written to the child's stdin. dumpvdl2 is launched as:

```
dumpvdl2 --iq-file - --sample-format S16_LE --oversample 1 \
         --centerfreq <chan_Hz> <chan_Hz> --output decoded:text:file:path=-
```

Since the VFO already centres the channel at DC, the channel frequency equals the
center frequency (zero mixer offset). dumpvdl2's text output is read back on a
separate thread and split into per-message blocks (each begins with a
`[timestamp] [freq] [… dBFS] …` header).

## Notes & limitations

- A clean, correctly tuned signal is needed; VDL2 bursts are short, so a good
  antenna and front-end help considerably.
- Encrypted or compressed payloads (notably some airline traffic) will not
  decode even with the full dumpvdl2 stack.
- Changing channel restarts the dumpvdl2 process (its frequency is set at
  launch). Starting, stopping and restarting the decoder are done on a
  background thread, so enabling the module or switching channel never freezes
  the SDR++ UI while the child process spins up or shuts down.

## Tests

`test/test_coproc.cpp` is a standalone harness (no SDR++ required) that drives a
real dumpvdl2 binary through the same `fork`/`exec`/`pipe` path the module uses,
feeding a synthetic VDL2 burst and checking the decoded output, plus verifying
the "binary not found" path. See the file header for the build/run command.

## License

This module drives dumpvdl2 (GPLv3) as an external process and is itself
distributed under the **GPLv3**.
