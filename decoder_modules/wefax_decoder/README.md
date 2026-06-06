# WEFAX Decoder for SDR++

An HF radiofacsimile (WEFAX / weather fax) decoder module for SDR++. It receives
and decodes WEFAX image transmissions directly inside SDR++, with automatic
phasing detection, automatic slant correction, and live image preview. It
decodes the same WEFAX signals as FLDIGI.

**Version 0.1.0**

## What is WEFAX

WEFAX (also called HF FAX or radiofax) is the analog facsimile mode used by
weather services to broadcast charts, satellite images, and forecasts on HF
(e.g. DWD Hamburg/Pinneberg, the UK Northwood marine fax, NOAA stations). The
image is frequency-modulated exactly like analog SSTV:

- **Black = 1500 Hz**, **white = 2300 Hz**, apex/center = 1900 Hz.

Because the modulation is identical to SSTV, this module reuses the SSTV
decoder's demodulation chain and slant-correction engine; the differences are in
the line geometry (IOC / LPM instead of a VIS-coded mode) and in the use of the
**phasing signal** as the timing reference instead of per-line sync pulses.

## Features

- **Standard WEFAX line rates**: 60, 90, 100, 120, 180, 240 LPM
  (lines per minute), default **120 LPM**.
- **Index Of Cooperation**: **IOC 576** (1810 px/line) and **IOC 288**
  (905 px/line), default IOC 576. Line width = round(IOC × π).
- **Three demodulation modes**: USB, LSB, and NFM, selectable to match how the
  signal is received (HF radiofax is almost always **USB**).
- **Automatic slant correction**: the decoder detects the **phasing pulses**
  (the white burst marking the left margin of each phasing line) and uses a
  linear regression with iterative outlier rejection to recover the true line
  period, compensating for clock-rate differences between the transmitter and
  the receiver. Correction is applied to both the live preview and the saved
  image. This is the same slant engine as the SSTV module.
- **Optional RANSAC slant calibration**: a more robust estimator for noisy or
  long transmissions, with an adaptive inlier tolerance that scales with the
  image length.
- **Calibration lock**: once the timing has converged to a low-jitter solution,
  it is frozen, so a late signal fade cannot disturb the already-decoded part of
  the image.
- **Automatic APT start/stop**: optionally listens for the APT start tone
  (300 Hz for IOC 576, 675 Hz for IOC 288), starts reception automatically and
  auto-selects the IOC; the 450 Hz stop tone finalizes and (optionally)
  auto-saves the image.
- **Manual trims**: a slant trim (ppm) and a horizontal shift (px) for fine
  alignment when auto-slant is disabled.
- **Optional 3×3 median filter**: light denoising applied to the final image.
- **Reception quality indicator**: shows a quality rating based on phasing
  timing jitter, plus the percentage of expected phasing pulses actually
  detected (useful for spotting signal loss during reception).
- **Image saving**: BMP, PNG, or JPEG output, with adjustable JPEG quality.
  Auto-save on completion, with a configurable output folder.
- **Cross-platform**: builds on Linux (and follows the standard SDR++ module
  layout for Windows).

## Line geometry

| IOC | Pixels / line | Typical use            |
|-----|---------------|------------------------|
| 576 | 1810          | Weather charts (most marine fax) |
| 288 | 905           | Lower-resolution charts |

| LPM | Seconds / line | Notes                  |
|-----|----------------|------------------------|
| 60  | 1.000          |                        |
| 90  | 0.667          |                        |
| 100 | 0.600          |                        |
| 120 | 0.500          | Most common worldwide  |
| 180 | 0.333          |                        |
| 240 | 0.250          |                        |

Most marine/aviation weather fax (DWD, Northwood, NOAA HF) uses **120 LPM,
IOC 576, USB**.

## Usage

1. Enable the **WEFAX** module in SDR++.
2. Set the **Demod** mode to match your reception:
   - **USB** for HF radiofax (the normal case).
   - **LSB** if your station inverts the sideband.
   - **NFM** for FM-relayed fax (rare).
3. Set **LPM** and **IOC** to match the station (120 / 576 for most charts), or
   enable **Auto-start (APT)** to let the start tone select them.
4. Tune so the image tones fall in the **1500–2300 Hz** audio range (the
   audio-center hint helps). The black/white frequencies are shown live.
5. Press **Force start** to begin immediately, or wait for the APT start tone.
   The image builds up line by line in the preview; slant is corrected
   automatically as phasing pulses accumulate.
6. The completed image is saved automatically (if **Auto-save** is enabled) to
   the configured folder, in the chosen format. **Reset** clears the image to
   start a new reception.

### Tuning tips

- **USB**: tune so a black area reads ~1500 Hz and white ~2300 Hz. A constant
  offset shows up as an overall brightness shift; the per-line geometry is
  unaffected.
- For **long charts**, enabling **RANSAC** improves robustness against timing
  drift. Watch the **syncs %** indicator: if it drops well below 100%, the
  receive chain is losing samples (RF fading or a loaded CPU), which corrupts
  the affected lines.
- If the image is sheared (diagonal), make sure **Auto slant** is on. If you
  prefer manual control, turn it off and adjust the **Slant (ppm)** and
  **H-shift (px)** trims.

## Building

The module follows the standard SDR++ out-of-tree module layout.

### Dependencies (Linux / Ubuntu 24)

```
sudo apt install cmake build-essential libglfw3-dev libfftw3-dev \
                 libvolk-dev libzstd-dev libglew-dev nlohmann-json3-dev
```

### Build

Place the `wefax_decoder` folder in the SDR++ `decoder_modules/` directory, add
it to the main build (an `option(OPT_BUILD_WEFAX_DECODER ...)` plus a matching
`add_subdirectory(...)` in the top-level `CMakeLists.txt`), then build SDR++ as
usual:

```
cd SDRPlusPlus/build
cmake ..
make wefax_decoder -j$(nproc)
```

The PNG/JPEG image writers are provided by a vendored copy of the public-domain
`stb_image_write.h`, so no extra image library is required.

### Installing the prebuilt module (Ubuntu 24)

A prebuilt `wefax_decoder.so` for Ubuntu 24 is included under
`prebuilt-ubuntu24/`. Copy it into the SDR++ plugins directory:

```
sudo cp wefax_decoder/prebuilt-ubuntu24/wefax_decoder.so /usr/lib/sdrpp/plugins/
```

Then enable the module from the SDR++ module manager.

## How it works

The signal flows through a demodulation chain (different per demod mode) that
produces an instantaneous-frequency stream — identical to the SSTV decoder. A
state machine then:

1. **Idle**: optionally listens for the APT **start tone** (black↔white square
   wave at 300 Hz for IOC 576 or 675 Hz for IOC 288). When detected, it
   auto-selects the IOC and starts reception. **Force start** bypasses this.
2. **Receiving**: records the raw frequency stream and detects the **phasing
   pulses** — the short white burst on each otherwise-black phasing line that
   marks the left edge.
3. Periodically **calibrates** the line period from the detected phasing-pulse
   positions (linear regression or RANSAC) and re-renders the image from the raw
   buffer, applying slant correction. Once the calibration converges to low
   jitter, it is **locked**.
4. **Done**: the APT **stop tone** (450 Hz) finalizes reception and triggers the
   optional auto-save.

Each instantaneous-frequency sample maps to a grayscale value
(1500 Hz → black, 2300 Hz → white), stored as RGB (R = G = B) for display and
saving.

## License

The vendored `stb_image_write.h` is public domain (see the header for details).

## Changelog

### v0.1.0

- Première version du module WEFAX pour SDR++.
- Démodulation USB / LSB / NFM réutilisée du module SSTV (chaîne DSP
  fréquence-instantanée identique : noir 1500 Hz, blanc 2300 Hz).
- Débits standard 60 / 90 / 100 / 120 / 180 / 240 LPM et IOC 576 / 288.
- Slant automatique par régression sur les impulsions de phasing (moteur
  identique à celui du module SSTV : régression linéaire robuste + RANSAC
  optionnel, verrouillage de calibration).
- Détection APT automatique (tonalité de départ 300/675 Hz avec auto-sélection
  de l'IOC, tonalité d'arrêt 450 Hz) ; démarrage manuel par « Force start ».
- Réglages manuels : slant (ppm) et décalage horizontal (px).
- Filtre médian 3×3 optionnel, indicateur de qualité de réception.
- Sauvegarde BMP / PNG / JPEG avec auto-save et dossier configurable.
- Interface identique à celle du module SSTV.
