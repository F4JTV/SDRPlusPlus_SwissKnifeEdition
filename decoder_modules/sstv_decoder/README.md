# SSTV Decoder for SDR++

An analog SSTV (Slow-Scan Television) decoder module for SDR++. It receives and
decodes SSTV image transmissions directly inside SDR++, with automatic mode
detection, automatic slant correction, and live image preview.

**Version 1.0.1**

## Compatibility

The source compiles against multiple SDR++ versions thanks to a compile-time
detection of the `dsp::demod::FM<float>::init()` signature. Both the older
4-argument variant (`in, rate, bw, lowPass`) and the newer 5-argument variant
(`in, rate, bw, lowPass, highPass`) are supported transparently; no manual
edits are required.

If you build against an even older SDR++ that lacks one of the DSP primitives
used here (`dsp::demod::Quadrature`, `dsp::convert::RealToComplex`,
`dsp::tap::FromArray`, etc.), update SDR++ to a recent release.

## Features

- **14 SSTV modes supported**, with automatic detection from the VIS code:
  - Martin: M1, M2
  - Scottie: S1, S2, DX
  - Robot: 36 Color, 72 Color
  - PD: PD50, PD90, PD120, PD160, PD180, PD240, PD290
- **Automatic mode detection** via the VIS header, or manual mode selection.
- **Three demodulation modes**: USB, LSB, and NFM, selectable to match how the
  signal is received.
- **Automatic slant correction**: the decoder detects the per-line sync pulses
  and uses a linear regression (with iterative outlier rejection) to recover the
  true line timing, compensating for clock-rate differences between the
  transmitter and the receiver. Correction is applied to both the live preview
  and the saved image.
- **Optional RANSAC slant calibration**: a more robust estimator for noisy or
  long transmissions, with an adaptive inlier tolerance that scales with the
  image length.
- **Calibration lock**: once the timing has converged to a low-jitter solution,
  it is frozen, so a late signal fade cannot disturb the already-decoded part of
  the image.
- **Weak-signal mode**: relaxed leader/break/sync detection thresholds for
  low-SNR transmissions (e.g. satellite or ISS reception).
- **Optional 3×3 median filter**: light denoising applied to the final image to
  remove isolated noise spikes.
- **Reception quality indicator**: shows a quality rating (Excellent / Good /
  Fair / Poor) based on sync timing jitter, plus the percentage of expected sync
  pulses actually detected (useful for spotting signal loss during reception).
- **Image saving**: BMP, PNG, or JPEG output, with adjustable JPEG quality.
  Auto-save on completion, with a configurable output folder.
- **Cross-platform**: builds on Linux and Windows.

## Supported modes and dimensions

| Mode        | Resolution | Approx. TX time | VIS (7-bit) |
|-------------|------------|-----------------|-------------|
| Martin M1   | 320 × 256  | 114 s           | 0x2C        |
| Martin M2   | 320 × 256  | 58 s            | 0x28        |
| Scottie S1  | 320 × 256  | 110 s           | 0x3C        |
| Scottie S2  | 320 × 256  | 71 s            | 0x38        |
| Scottie DX  | 320 × 256  | 269 s           | 0x4C        |
| Robot 36    | 320 × 240  | 36 s            | 0x08        |
| Robot 72    | 320 × 240  | 72 s            | 0x0C        |
| PD50        | 320 × 256  | 50 s            | 0x5D        |
| PD90        | 320 × 256  | 90 s            | 0x63        |
| PD120       | 640 × 496  | 126 s           | 0x5F        |
| PD160       | 512 × 400  | 161 s           | 0x62        |
| PD180       | 640 × 496  | 187 s           | 0x60        |
| PD240       | 640 × 496  | 248 s           | 0x61        |
| PD290       | 800 × 616  | 289 s           | 0x5E        |

## Usage

1. Enable the **SSTV** module in SDR++.
2. Set the **Demod** mode to match your reception:
   - **USB** / **LSB** for HF SSTV (tune to the suppressed-carrier edge).
   - **NFM** for VHF/UHF FM SSTV (tune so the FM carrier is centered in the VFO).
3. Leave **Mode** on **Auto (VIS)** for automatic detection, or pick a specific
   mode to force it.
4. When a transmission is detected, the image builds up line by line in the
   preview. Slant is corrected automatically as sync pulses accumulate.
5. The completed image is saved automatically (if **Auto-save** is enabled) to
   the configured folder, in the chosen format.

### Tuning tips

- **USB/LSB**: the audio-center frequency is configurable; tune so the image
  frequencies fall in the 1500–2300 Hz range.
- **NFM**: keep the FM carrier centered in the VFO for clean colors.
- For **long modes** (PD180/240/290, Scottie DX), enabling **RANSAC** improves
  robustness against timing drift. Watch the **syncs %** indicator: if it drops
  well below 100%, the receive chain is losing samples (e.g. due to RF fading or
  a heavily loaded CPU), which corrupts the affected lines.

## Building

The module follows the standard SDR++ out-of-tree module layout.

### Dependencies (Linux / Ubuntu)

```
sudo apt install cmake build-essential libglfw3-dev libfftw3-dev \
                 libvolk-dev libzstd-dev libglew-dev nlohmann-json3-dev
```

### Build

Place the `sstv_decoder` folder in the SDR++ `decoder_modules/` directory, add
it to the main build (an `option(OPT_BUILD_SSTV_DECODER ...)` plus a matching
`add_subdirectory(...)` in the top-level `CMakeLists.txt`), then build SDR++ as
usual:

```
cd SDRPlusPlus/build
cmake ..
make sstv_decoder -j$(nproc)
```

The PNG/JPEG image writers are provided by a vendored copy of the public-domain
`stb_image_write.h`, so no extra image library is required.

### Installing the prebuilt module (Ubuntu)

A prebuilt `sstv_decoder.so` for Ubuntu is included under
`prebuilt-ubuntu24/`. Copy it into the SDR++ plugins directory:

```
sudo cp sstv_decoder/prebuilt-ubuntu24/sstv_decoder.so /usr/lib/sdrpp/plugins/
```

Then enable the module from the SDR++ module manager.

## How it works

The signal flows through a demodulation chain (different per demod mode) that
produces an instantaneous-frequency stream. A state machine then:

1. **Hunts** for the 1900 Hz leader tone.
2. Detects the **leader → break** transition.
3. Decodes the 7-bit **VIS** code (start bit, 7 data bits LSB-first, even parity,
   stop bit) to identify the mode.
4. **Receives the image**, recording the raw frequency stream and detecting the
   per-line sync pulses.
5. Periodically calibrates the line timing from the detected sync positions and
   re-renders the image from the raw buffer, applying slant correction. Once the
   calibration converges, it is locked.

Color is reconstructed per the mode's color model: GBR sub-scans for
Martin/Scottie, and Y/R-Y/B-Y (BT.601 YUV) for the PD and Robot families.

## License

The vendored `stb_image_write.h` is public domain (see the header for details).
