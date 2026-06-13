# hell_decoder — Hellschreiber decoder for SDR++

An in-tree SDR++ decoder module for the **Hellschreiber** ("Hell") facsimile
text modes. Hell is a column-scan image mode: characters are painted as a
vertical raster, so the module shows a **horizontally scrolling image** (newest
on the right) rather than decoded text — exactly how the mode is read on air.

The receive path is a faithful port of the FLDIGI `feld` modem (W1HKJ, GPLv3,
itself derived from gmfsk). The DSP front end (sideband selection, squelch,
audio handling) follows the same conventions as the other FLDIGI-family modules
in this fork (MFSK / RTTY / PSK).

## Modes

| Mode          | Type | Column rate | Occupied BW | Notes                        |
|---------------|------|------------:|------------:|------------------------------|
| Feld Hell     | AM   | 17.5 col/s  | 245 Hz      | The classic on/off-keyed Hell|
| Slow Hell     | AM   | 2.1875 col/s| ~31 Hz      | 1/8 speed, very narrow        |
| Feld Hell X5  | AM   | 87.5 col/s  | 1225 Hz     | 5× speed                      |
| Feld Hell X9  | AM   | 157.5 col/s | 2205 Hz     | 9× speed                      |
| FSK Hell-245  | FSK  | 17.5 col/s  | 122.5 Hz    | Frequency-keyed variant       |
| FSK Hell-105  | FSK  | 17.5 col/s  | 55 Hz       | Narrow FSK variant            |
| Hell 80       | FSK  | 35 col/s    | 300 Hz      | Faster FSK variant            |

The amplitude modes (Feld Hell and its X5/X9/Slow speeds) are demodulated with an
envelope detector; the FSK modes use a frequency discriminator. Mode parameters
(column rate, bandwidth, baseband filter widths) match FLDIGI exactly.

## Signal chain

```
  VFO IQ ─▶ PowerSquelch ─▶ SSB(USB/LSB) or FM(NFM) ─▶ real audio @ 8 kHz
        ─▶ Hilbert (analytic) ─▶ NCO mix to baseband ─▶ complex low-pass
        ─▶ AM envelope  ┐
                        ├─▶ downsample to pixel rate ─▶ AGC / peak-hold
        ─▶ FM discrim.  ┘
        ─▶ pack column (RxColumnLen pixels) ─▶ emit 2×height strip
        ─▶ scrolling grayscale raster ─▶ GL texture
```

The VFO is created at **8 kHz** (FLDIGI's native `feld` sample rate), so the
audio reaches the modem at the rate it expects with no extra resampling.

## Using it

1. Tune to the Hell signal and place the VFO on it. Pick the **sideband**
   (USB is the usual choice on HF), then select the **Mode**.
2. Use the **band view** (click/drag) or the **AF freq** slider to put the
   yellow centre marker on the carrier. The shaded band shows the expected
   occupied width for the selected mode.
3. The image scrolls in as columns arrive. Adjust:
   - **RX height** — vertical resolution of the raster (14–42 px, FLDIGI's
     `HellRcvHeight`). Higher = taller, more detailed glyphs.
   - **AGC** — Slow / Medium / Fast envelope tracking (FLDIGI `hellagc`).
   - **Reverse** — swap mark/space (FSK modes).
   - **Blackboard** — invert to white-on-black.
   - **Squelch** — gate painting on the signal metric so noise doesn't fill the
     raster between transmissions.
4. **Save image** writes the current raster to PNG or JPG in the chosen folder.
   **Clear** wipes the raster.

> Tip: the raster is drawn with the classic Hell vertical duplication (each
> column is shown stacked twice), so text stays readable even when it is not
> vertically centred.

## Building

The module is wired into the SDR++ build like the other decoders:

```cmake
option(OPT_BUILD_HELL_DECODER "Build the Hellschreiber decoder module" ON)
...
if (OPT_BUILD_HELL_DECODER)
add_subdirectory("decoder_modules/hell_decoder")
endif (OPT_BUILD_HELL_DECODER)
```

Then configure and build as usual:

```bash
cd build
cmake .. -DOPT_BUILD_HELL_DECODER=ON
make hell_decoder -j$(nproc)
```

The compiled plugin is `hell_decoder.so`. A prebuilt copy for Ubuntu 24.04 is
included under `prebuilt-ubuntu24/`. To install it into a running SDR++:

```bash
sudo cp prebuilt-ubuntu24/hell_decoder.so /usr/lib/sdrpp/plugins/
```

Then enable **hell_decoder** in *Module Manager* and add an instance.

## Configuration

Settings are persisted per instance in `hell_decoder_config.json`
(mode, sideband, snap, AF, RX height, AGC, reverse, blackboard, squelch,
image format/quality, save folder).

## Files

```
hell_decoder/
├── CMakeLists.txt
├── README.md
├── prebuilt-ubuntu24/
│   └── hell_decoder.so
└── src/
    ├── decoder.h            # base control-surface interface
    ├── main.cpp             # module, GUI, GL image display, save
    ├── stb_image_write.h    # PNG/JPG export
    ├── feld/
    │   └── feld.h           # ported FLDIGI feld RX modem (self-contained DSP)
    └── hell/
        └── decoder.h        # front-end DSP chain (squelch → SSB/FM → modem)
```

## Credits & licence

The receive modem is a port of the FLDIGI `feld` modem by Dave Freese (W1HKJ),
derived from gmfsk (OH2BNS / VE7IT), licensed under the **GNU GPL v3**. This
module is distributed under the same terms.
