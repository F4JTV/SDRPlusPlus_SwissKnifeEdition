# GPS L1 C/A Decoder Module for SDR++

A decoder module for SDR++ that processes GPS L1 C/A signals at **1575.42 MHz**. It performs full signal acquisition, code/carrier tracking, and 50 bps navigation data demodulation on up to 12 satellites in parallel.

## Features

- **Parallel code-phase acquisition** using FFT-based circular correlation (FFTW3)
- **Doppler search** over ±5 kHz with 500 Hz bin spacing
- **Coherent tracking** with independent DLL (Delay-Locked Loop) and Costas PLL per channel
- **C/N0 estimation** using the Beaulieu method (200 ms window)
- **Bit synchronization** and 50 bps NAV message demodulation
- **Subframe detection** with preamble (0x8B), TLM and HOW word parsing, exposing TOW (Time Of Week), alert flag, A-S flag, and subframe ID (1–5)
- **GUI**: live tracking channel table with PRN, Doppler, C/N0, lock indicator, bit count, and last decoded subframe ID; acquisition snapshot table; raw subframe log panel
- **All 32 GPS PRN codes** generated from the official IS-GPS-200 G2 phase taps; correctness verified bit-for-bit against the standard

## Dependencies (Ubuntu 24.04)

Install the SDR++ build dependencies plus FFTW3:

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential cmake git pkg-config \
    libfftw3-dev \
    libglfw3-dev libvolk2-dev libzstd-dev \
    libusb-1.0-0-dev libfmt-dev libsoapysdr-dev libairspy-dev \
    libhackrf-dev librtlsdr-dev libairspyhf-dev libbladerf-dev \
    libiio-dev libad9361-dev libcodec2-dev libportaudio2 portaudio19-dev
```

## Building

1. Clone the official SDR++ source:

   ```bash
   git clone https://github.com/AlexandreRouma/SDRPlusPlus.git
   cd SDRPlusPlus
   ```

2. Copy this module into the SDR++ source tree:

   ```bash
   cp -r /path/to/gps_decoder decoder_modules/gps_decoder
   ```

3. Edit the top-level `CMakeLists.txt` of SDR++ and add the option plus the subdirectory hook:

   ```cmake
   option(OPT_BUILD_GPS_DECODER "Build the GPS L1 C/A decoder module" ON)
   # ...
   if (OPT_BUILD_GPS_DECODER)
       add_subdirectory("decoder_modules/gps_decoder")
   endif (OPT_BUILD_GPS_DECODER)
   ```

4. Configure and build:

   ```bash
   mkdir build && cd build
   cmake -DOPT_BUILD_GPS_DECODER=ON ..
   make -j$(nproc)
   sudo make install
   ```

## Usage

1. Connect a GPS-capable antenna. **An active antenna with an LNA is strongly recommended** — GPS signals reach the ground at around −130 dBm, well below the thermal noise floor. A clear sky view is required; the module will not acquire indoors or with most non-GNSS whips.
2. Start SDR++.
3. Open **Module Manager**, add a new instance with type `gps_decoder` (give it any name).
4. Tune your SDR to a center frequency that includes 1575.42 MHz. The module ships with a **"Tune to L1"** button that retunes the active SDR directly to 1575.420 MHz.
5. The internal VFO is locked at **2.048 MHz sample rate / 2 MHz bandwidth**, centered on the L1 carrier.
6. Acquisition starts automatically. Within a few seconds, satellites in view should appear in the tracking table.

## GUI controls

- **Tune to L1** — sets the SDR center frequency to 1575.420 MHz.
- **Acq. threshold** — peak-to-secondary-peak ratio required to declare a PRN acquired. Default `2.5`. Raise it (3.0–3.5) if you get false detections under noise; lower it (2.0–2.3) if real satellites are missed.
- **Acq. period** — interval in seconds between acquisition sweeps. Default `2.0 s`. Each sweep scans all 32 PRNs that are not currently being tracked.
- **Acquire now** — force an immediate acquisition sweep.
- **Reset channels** — drop all currently tracked PRNs and start over.
- **Tracking channels table** — one row per active PRN. C/N0 cell is color-coded (green ≥ 40 dB-Hz, yellow ≥ 35, orange ≥ 30, red below).
- **Last acquisition** — snapshot of the most recent sweep showing detected PRNs, code phase, and Doppler.
- **Subframe log** — every decoded subframe (PRN, subframe ID, TOW count, flags).

## What this module does and does not do

This module covers the **signal-processing side** of a GPS receiver up to and including navigation bit demodulation and subframe framing. It exposes the TOW, alert flag, A-S flag, and subframe ID from the HOW word of each decoded subframe.

It **does not** compute a position fix. Producing a PVT (Position/Velocity/Time) solution additionally requires:

- Full ephemeris extraction (subframes 1, 2, 3) — Keplerian elements, clock corrections, ionospheric model.
- Pseudorange estimation from the precise code phase and TOW.
- Iterative least-squares or Kalman positioning across ≥4 satellites.

For a full PVT implementation in C++, see the reference open-source project **GNSS-SDR** (https://gnss-sdr.org/). This module is a focused educational and signal-monitoring tool, not a competitor to a full GNSS receiver.

## Technical parameters

| Parameter | Value |
|---|---|
| Center frequency | 1575.42 MHz (GPS L1) |
| Sample rate | 2.048 MHz |
| VFO bandwidth | 2.0 MHz |
| Code length | 1023 chips |
| Code rate | 1.023 Mcps |
| Code period | 1 ms (2048 samples) |
| Acquisition method | FFT parallel code-phase search |
| Doppler search range | ±5 kHz |
| Doppler search step | 500 Hz |
| Tracking — DLL | Early/Prompt/Late, 0.5 chip spacing, non-coherent envelope discriminator |
| Tracking — PLL | Costas atan(Q/I) discriminator, 2nd-order loop filter |
| NAV bit rate | 50 bps (20 ms / bit) |
| Subframe length | 300 bits / 6 seconds |
| Preamble | 0x8B (8 bits) |

## Troubleshooting

**No satellites acquired.**
- Check the antenna: GPS antennas need an LNA and bias-tee, or active USB GPS dongles. A plain wire or VHF/UHF antenna at L1 will give essentially nothing.
- Check sky view: ideally outdoors with most of the sky visible. Indoors near a window may work but is unreliable.
- Verify the SDR is actually tuned to 1575.42 MHz with the **Tune to L1** button.
- Lower the acquisition threshold to ~2.0 and click **Acquire now**.

**Channels lock briefly then drop.**
- The SDR clock drift may be high. RTL-SDR units especially benefit from a TCXO variant. Standard ±50 ppm dongles can struggle at L1; ±0.5 ppm TCXOs are preferred.
- C/N0 below ~30 dB-Hz typically results in cycle slips in the Costas loop.

**Bit count rises but no subframes appear.**
- Wait. Subframes are 6 seconds long; the bit synchronizer needs around 1 second of stable tracking before locking onto the 20 ms boundary, then up to 12 seconds to verify two consecutive preambles spaced exactly 300 bits apart.
- If C/N0 is borderline, bit errors will prevent the preamble from being recognized.

**Module not visible in Module Manager.**
- Confirm the build installed `gps_decoder.so` (Linux) into the SDR++ modules directory.
- Check the SDR++ console for module load errors at startup.

## License

Released under the terms compatible with SDR++ itself (GPL-3.0). See the SDR++ project for full license text.

## Credits

- **SDR++** by Alexandre Rouma (https://github.com/AlexandreRouma/SDRPlusPlus)
- **IS-GPS-200** (Interface Specification, US Space Force) — the authoritative reference for GPS L1 C/A signal structure, navigation message, and PRN code phase taps.
- **GNSS-SDR** (https://gnss-sdr.org/) — reference open-source GNSS receiver, used as inspiration for the peak-to-secondary-peak acquisition metric and the Beaulieu C/N0 estimator.
- **FFTW3** (http://www.fftw.org/) — the FFT library underlying the acquisition engine.
