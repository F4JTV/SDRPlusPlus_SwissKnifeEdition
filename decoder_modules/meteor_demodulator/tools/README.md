# Meteor LRPT test-signal generator

`meteor_lrpt_gen` builds a **complete, decodable Meteor LRPT signal** from a
generated test image and writes it as a **CS8 IQ file** (interleaved int8, the
HackRF format). Use it to test the `meteor_demodulator` LRPT decoder without
waiting for a satellite pass.

It is the exact inverse of the decoder and reuses its ported codings
(Reed-Solomon, convolutional encoder, CCSDS randomization, CCSDS headers,
MSU-MR Huffman/quantization tables). The MSU-MR image is encoded with a forward
DCT + JPEG-style Huffman, wrapped in CCSDS packets (APID 64/65/66 → channels
1/2/3), framed into VCID-5 CADUs with RS(255,223) i=4 and CCSDS randomization,
convolutionally encoded (r=1/2 k=7), and QPSK/OQPSK pulse-shaped with an RRC
filter (β=0.6).

## Build
```
cd decoder_modules/meteor_demodulator/tools
./build_gen.sh
```
(Adjust the `LC` path in the script to your SDR++ `core/libcorrect` if needed.)

## Use
```
# Meteor-M2 legacy (72k QPSK):
./meteor_lrpt_gen --mode legacy --lines 60 --qf 60 --samplerate 2304000 -o meteor_legacy.cs8

# Meteor-M2-3 / M2-4 (M2-x: NRZ-M + OQPSK):
./meteor_lrpt_gen --mode m2x    --lines 60 --qf 60 --samplerate 2304000 -o meteor_m2x.cs8
```
Options: `--lines N` image height in 8-px groups, `--qf 1..100` JPEG quality,
`--samplerate Hz` (use ≥2 MHz for HackRF; an **integer** samples/symbol is best,
e.g. 2304000 = 32×72k), `--symrate Hz` (default 72000), `--selftest` writes raw
soft symbols instead of CS8 (for the loopback harness).

## Transmit / replay
* **HackRF (RF loopback into a dummy load / attenuator, never a live antenna):**
  ```
  hackrf_transfer -t meteor_m2x.cs8 -f 137900000 -s 2304000 -x 10 -a 0
  ```
  Tune SDR++ to 137.900 MHz and decode. Keep TX gain low and use a dummy load —
  137.9 MHz is a protected satellite band; do not radiate.
* **No hardware — SDR++ file source:** load the `.cs8` as a Complex Int8
  baseband file at the matching sample rate, centered at 0 Hz.

Then in the meteor_demodulator module: pick the matching **Satellite** mode
(M2-3 for `--mode m2x`, "Meteor-M2 legacy" for `--mode legacy`), tick
**Decode LRPT (live)**, and an image should build within seconds.

## Validation
Both paths were verified by software loopback (generator → decoder): the decoded
image reproduces the test pattern, with `frames == CADUs` and zero RS errors.
