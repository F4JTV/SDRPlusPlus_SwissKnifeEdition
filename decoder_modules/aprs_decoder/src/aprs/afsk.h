#pragma once
#include <stdint.h>
#include <math.h>
#include <vector>
#include <functional>

// ---------------------------------------------------------------------------
//  AFSK 1200 (Bell 202) demodulator + bit synchronizer + HDLC deframer
//
//  Input  : FM-demodulated real audio at a fixed sample rate (24 kHz)
//  Output : complete, FCS-valid AX.25 frames via a callback
//
//  Chain  : non-coherent dual-tone correlator (1200 Hz mark / 2200 Hz space)
//           -> digital PLL bit sync at 1200 baud
//           -> NRZI decode
//           -> HDLC flag detect + bit de-stuffing + byte assembly (LSB first)
//           -> CRC-16/X.25 (FCS) check
//
//  The correlator + sphase clock-recovery logic follows the classic, well
//  documented Bell-202 soft-decoder design (multimon-ng / soundmodem family),
//  re-implemented here in a clean, self-contained form. No external deps.
// ---------------------------------------------------------------------------

namespace aprs {

    static constexpr double AFSK_SAMPLE_RATE = 24000.0; // exactly 20 samples / bit
    static constexpr double AFSK_BAUD        = 1200.0;
    static constexpr double AFSK_MARK_HZ     = 1200.0;
    static constexpr double AFSK_SPACE_HZ    = 2200.0;
    static constexpr int    CORRLEN          = (int)(AFSK_SAMPLE_RATE / AFSK_BAUD); // 20

    // 16-bit fractional phase increment per audio sample, used by the bit-sync PLL
    static constexpr uint32_t SPHASEINC = (uint32_t)(0x10000u * AFSK_BAUD / AFSK_SAMPLE_RATE); // 3276

    // CRC-16/X.25 (reflected 0x1021 -> 0x8408), used as the AX.25 frame FCS.
    inline uint16_t crc16_x25(const uint8_t* data, size_t len) {
        uint16_t crc = 0xFFFF;
        for (size_t i = 0; i < len; i++) {
            crc ^= data[i];
            for (int b = 0; b < 8; b++) {
                if (crc & 1) { crc = (crc >> 1) ^ 0x8408; }
                else         { crc >>= 1; }
            }
        }
        return (uint16_t)(crc ^ 0xFFFF);
    }

    class AFSK1200 {
    public:
        // Callback receives a complete, FCS-validated AX.25 frame
        // (address + control + pid + info; the 2 FCS bytes are stripped).
        using FrameHandler = std::function<void(const uint8_t* frame, int len)>;

        AFSK1200() {
            // Precompute the in-phase / quadrature correlation kernels for both tones.
            for (int i = 0; i < CORRLEN; i++) {
                double phMark  = 2.0 * M_PI * AFSK_MARK_HZ  * i / AFSK_SAMPLE_RATE;
                double phSpace = 2.0 * M_PI * AFSK_SPACE_HZ * i / AFSK_SAMPLE_RATE;
                corrMarkI[i]  = (float)cos(phMark);
                corrMarkQ[i]  = (float)sin(phMark);
                corrSpaceI[i] = (float)cos(phSpace);
                corrSpaceQ[i] = (float)sin(phSpace);
            }
            reset();
        }

        void setFrameHandler(const FrameHandler& h) { onFrame = h; }

        void reset() {
            for (int i = 0; i < CORRLEN; i++) { ring[i] = 0.0f; }
            ringPos = 0;
            sphase = 0;
            lastLevel = 0;
            // HDLC
            rawreg = 0;
            bitbuf = 0;
            numbits = 0;
            inFrame = false;
            frame.clear();
        }

        // Feed a block of real audio samples (24 kHz). Emits frames as they complete.
        void process(const float* audio, int count) {
            for (int n = 0; n < count; n++) {
                // --- push sample into the correlator ring buffer ---
                ring[ringPos] = audio[n];
                ringPos = (ringPos + 1) % CORRLEN;

                // --- dual-tone correlation over one bit period ---
                float mi = 0, mq = 0, si = 0, sq = 0;
                int idx = ringPos; // oldest sample
                for (int k = 0; k < CORRLEN; k++) {
                    float s = ring[idx];
                    mi += s * corrMarkI[k];
                    mq += s * corrMarkQ[k];
                    si += s * corrSpaceI[k];
                    sq += s * corrSpaceQ[k];
                    idx = (idx + 1) % CORRLEN;
                }
                float markMag  = mi * mi + mq * mq;
                float spaceMag = si * si + sq * sq;
                int level = (markMag > spaceMag) ? 1 : 0; // demodulated tone level

                // --- bit-sync PLL (digital, 16-bit fractional phase) ---
                if (level != lastLevel) {
                    // Tone transition: nudge sampling phase toward bit center.
                    if (sphase < 0x8000u) { sphase += SPHASEINC / 8; }
                    else                  { sphase -= SPHASEINC / 8; }
                }
                lastLevel = level;

                sphase += SPHASEINC;
                if (sphase >= 0x10000u) {
                    sphase &= 0xFFFFu;
                    rxbit(level); // sample at (recovered) bit center
                }
            }
        }

    private:
        // ----- NRZI decode + HDLC framing on the recovered bit clock -----
        void rxbit(int level) {
            // NRZI: 1 = no transition, 0 = transition.
            int bit = (level == nrziLast) ? 1 : 0;
            nrziLast = level;
            hdlcBit(bit);
        }

        void hdlcBit(int bit) {
            // Raw register used for flag / stuffing pattern matching.
            rawreg = (rawreg << 1) | (bit & 1);

            // Flag = 0x7E (01111110): frame boundary.
            if ((rawreg & 0xFF) == 0x7E) {
                if (inFrame) { tryEmit(); }
                inFrame = true;
                bitbuf = 0; numbits = 0; frame.clear();
                return;
            }
            if (!inFrame) { return; }

            // Abort: 7+ consecutive ones.
            if ((rawreg & 0x7F) == 0x7F) {
                inFrame = false; bitbuf = 0; numbits = 0; frame.clear();
                return;
            }

            // Bit de-stuffing: last six bits == 111110 -> drop the stuffed zero.
            if ((rawreg & 0x3F) == 0x3E) { return; }

            // Store data bit, LSB first within each byte (AX.25 convention).
            bitbuf >>= 1;
            if (bit) { bitbuf |= 0x80; }
            if (++numbits >= 8) {
                frame.push_back(bitbuf);
                bitbuf = 0; numbits = 0;
                if (frame.size() > 512) { // runaway guard
                    inFrame = false; frame.clear();
                }
            }
        }

        void tryEmit() {
            // Minimum valid AX.25: 14 (addr) + 1 (ctrl) + 2 (FCS) = 17 bytes.
            if (frame.size() < 17) { return; }
            size_t n = frame.size();
            uint16_t fcsCalc = crc16_x25(frame.data(), n - 2);
            uint16_t fcsRecv = (uint16_t)(frame[n - 2] | (frame[n - 1] << 8));
            if (fcsCalc != fcsRecv) { return; } // CRC fail -> silently drop
            if (onFrame) { onFrame(frame.data(), (int)(n - 2)); } // strip FCS
        }

        // Correlator
        float ring[CORRLEN];
        int   ringPos = 0;
        float corrMarkI[CORRLEN], corrMarkQ[CORRLEN];
        float corrSpaceI[CORRLEN], corrSpaceQ[CORRLEN];

        // Bit sync
        uint32_t sphase = 0;
        int      lastLevel = 0;

        // NRZI
        int nrziLast = 0;

        // HDLC
        uint32_t rawreg = 0;
        uint8_t  bitbuf = 0;
        int      numbits = 0;
        bool     inFrame = false;
        std::vector<uint8_t> frame;

        FrameHandler onFrame;
    };

}
