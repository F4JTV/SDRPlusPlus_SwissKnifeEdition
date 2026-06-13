# FreeDV decoder — sample files

These files were produced with the test harness in `../src/test` from a short
8 kHz speech clip (`ve9qrp_10s.raw`, from the codec2 project). Each transmission
embeds the repeating text-channel message `FREEDV TEST `.

## `modem/` — 8 kHz modem-audio WAVs

These are the audio an SSB receiver would produce. Decode them directly with the
test harness, or play them into the FreeDV reference application.

| File                          | Mode  | Channel SNR | Expected result            |
|-------------------------------|-------|-------------|----------------------------|
| `freedv_700D_8k.wav`          | 700D  | clean       | sync, text recovered       |
| `freedv_700E_8k.wav`          | 700E  | clean       | sync, text recovered       |
| `freedv_1600_8k.wav`          | 1600  | clean       | sync, text recovered       |
| `freedv_700E_8k_snr0dB.wav`   | 700E  | 0 dB        | still syncs (robust mode)  |

```sh
cd ../src/test
./test_decode 700D ../../samples/modem/freedv_700D_8k.wav "FREEDV TEST"
./test_decode 700E ../../samples/modem/freedv_700E_8k_snr0dB.wav "FREEDV TEST"
```

## `iq/` — 48 kHz complex IQ WAVs (for the SDR++ File source)

Each file carries the FreeDV signal as **USB at +6000 Hz** from the recording
centre. Open with the SDR++ **File** source, move the VFO to the +6000 Hz
signal, select the matching mode in the FreeDV Decoder, and you should get sync.

| File                       | Mode  |
|----------------------------|-------|
| `freedv_700D_iq_48k.wav`   | 700D  |
| `freedv_700E_iq_48k.wav`   | 700E  |

To regenerate, see the `make_iq.py` section of the main `README.md`.
