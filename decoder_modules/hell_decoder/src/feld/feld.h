#pragma once
#include <complex>
#include <vector>
#include <mutex>
#include <cmath>
#include <cstdint>
#include <atomic>
#include <algorithm>

// ---------------------------------------------------------------------------
//  feld.h  --  Hellschreiber (FELDHELL family) receive modem
//
//  Faithful port of the FLDIGI `feld` modem receive path (src/feld/feld.cxx,
//  W1HKJ, GPLv3, itself derived from gmfsk by OH2BNS / VE7IT). Only the RX path
//  is ported; the TX path, fonts and FLTK raster are not needed here.
//
//  Signal chain, identical to FLDIGI (native rate = 8000 Hz):
//     real audio -> Hilbert (analytic signal) -> NCO mix to baseband
//                -> complex low-pass (filter_bandwidth)
//                -> AM envelope  [rx()]   for the amplitude modes
//                -> FM discriminator [FSKH_rx()] for the FSK modes
//                -> downsample to rxpixrate -> AGC / peak-hold
//                -> pack RxColumnLen pixels into a column, emit a
//                   2*RxColumnLen-tall strip (previous + current column,
//                   the classic Hell vertical-duplication "marquee")
//
//  The strips are written into an internal horizontally-scrolling grayscale
//  ring; getImageRGB() linearises the most recent columns (newest on the
//  right) for upload to a GL texture by the GUI, mirroring the wefax module.
// ---------------------------------------------------------------------------

namespace hell {

    typedef std::complex<float> cmplx;

    static constexpr double FELD_SAMPLE_RATE = 8000.0;
    static constexpr int    FELD_COLUMN_LEN  = 14;     // TX font height (FLDIGI)
    static constexpr int    MAX_RX_COLUMN_LEN = 42;    // FLDIGI HellRcvHeight max
    static constexpr int    HELL_IMG_COLS    = 2400;   // visible scroll width

    enum HellModeId {
        MODE_FELDHELL = 0,  // Feld Hell
        MODE_SLOWHELL,      // Slow Hell
        MODE_HELLX5,        // Feld Hell X5
        MODE_HELLX9,        // Feld Hell X9
        MODE_FSKH245,       // FSK Hell-245
        MODE_FSKH105,       // FSK Hell-105
        MODE_HELL80,        // Hell 80
        HELL_MODE_COUNT
    };

    struct HellModeDef {
        const char* name;
        double      feldcolumnrate;  // columns per second
        bool        fsk;             // true: FSK discriminator, false: AM envelope
        double      hell_bandwidth;  // occupied bandwidth used for the baseband LPF
    };

    // Order matches the FLDIGI menu (and the requested dropdown).
    static const HellModeDef HELL_MODES[HELL_MODE_COUNT] = {
        { "Feld Hell",    17.5,    false, 245.0   },  // hb = 14 * 17.5
        { "Slow Hell",    2.1875,  false, 30.625  },  // hb = 14 * 2.1875
        { "Feld Hell X5", 87.5,    false, 1225.0  },  // hb = 14 * 87.5
        { "Feld Hell X9", 157.5,   false, 2205.0  },  // hb = 14 * 157.5
        { "FSK Hell-245", 17.5,    true,  122.5   },
        { "FSK Hell-105", 17.5,    true,  55.0    },
        { "Hell 80",      35.0,    true,  300.0   },
    };

    // FLDIGI's AGC decay options (progdefaults.hellagc): 1 slow, 2 medium, 3 fast.
    enum HellAgc { HELL_AGC_SLOW = 1, HELL_AGC_MEDIUM = 2, HELL_AGC_FAST = 3 };

    // --- small DSP helpers (self-contained, no FLDIGI dependencies) ----------

    // Moving average (FLDIGI Cmovavg equivalent).
    class MovAvg {
    public:
        void setLength(int len) {
            len = std::max(1, len);
            if (len != (int)buf.size()) { buf.assign(len, 0.0); }
            else { std::fill(buf.begin(), buf.end(), 0.0); }
            idx = 0; sum = 0.0; filled = false;
        }
        double run(double v) {
            if (buf.empty()) return v;
            sum += v - buf[idx];
            buf[idx] = v;
            if (++idx >= (int)buf.size()) { idx = 0; filled = true; }
            int n = filled ? (int)buf.size() : idx;
            return sum / (n ? n : 1);
        }
        void clear() { std::fill(buf.begin(), buf.end(), 0.0); idx = 0; sum = 0.0; filled = false; }
    private:
        std::vector<double> buf;
        int    idx = 0;
        double sum = 0.0;
        bool   filled = false;
    };

    // 37-tap Hilbert transformer -> analytic signal. The real path is delayed by
    // the filter group delay so I and Q are time-aligned (equivalent to FLDIGI's
    // C_FIR_filter::init_hilbert(37,1) + run(z,z)).
    class HilbertFIR {
    public:
        HilbertFIR() { build(); }
        void reset() { std::fill(dline.begin(), dline.end(), 0.0f); pos = 0; }
        // in: real sample, out: analytic sample (I delayed, Q = hilbert)
        cmplx run(float in) {
            dline[pos] = in;
            float q = 0.0f;
            int p = pos;
            for (int k = 0; k < NTAPS; k++) {
                q += taps[k] * dline[p];
                if (--p < 0) p += NTAPS;
            }
            // delayed real (centre tap delay)
            int dp = pos - GROUP_DELAY; if (dp < 0) dp += NTAPS;
            float i = dline[dp];
            if (++pos >= NTAPS) pos = 0;
            return cmplx(i, q);
        }
    private:
        static constexpr int NTAPS = 37;
        static constexpr int GROUP_DELAY = (NTAPS - 1) / 2;
        float taps[NTAPS];
        std::vector<float> dline;
        int pos = 0;
        void build() {
            dline.assign(NTAPS, 0.0f);
            // ideal Hilbert h[n] = (2/pi)/n for odd n, 0 for even; Hamming window.
            int M = GROUP_DELAY;
            for (int n = 0; n < NTAPS; n++) {
                int k = n - M;
                double h;
                if (k == 0 || (k % 2) == 0) h = 0.0;
                else h = 2.0 / (M_PI * k);
                double w = 0.54 - 0.46 * cos(2.0 * M_PI * n / (NTAPS - 1));
                taps[n] = (float)(h * w);
            }
        }
    };

    // Complex low-pass: a windowed-sinc real-tap FIR applied to I and Q. Replaces
    // FLDIGI's fftfilt baseband low-pass; cutoff = filter_bandwidth (Hz).
    class ComplexLPF {
    public:
        void design(double cutoffHz, double sampleRate) {
            double fc = std::clamp(cutoffHz / sampleRate, 0.0005, 0.49); // 1.0=fs
            int n = (int)std::ceil(4.0 / fc) | 1;                        // odd
            n = std::clamp(n, 31, 511);
            taps.assign(n, 0.0f);
            int M = (n - 1) / 2;
            double sum = 0.0;
            for (int i = 0; i < n; i++) {
                int k = i - M;
                double s = (k == 0) ? (2.0 * fc)
                                    : sin(2.0 * M_PI * fc * k) / (M_PI * k);
                double w = 0.42 - 0.5 * cos(2.0 * M_PI * i / (n - 1))
                                + 0.08 * cos(4.0 * M_PI * i / (n - 1)); // Blackman
                taps[i] = (float)(s * w);
                sum += taps[i];
            }
            for (auto& t : taps) t /= (float)sum;
            re.assign(n, 0.0f); im.assign(n, 0.0f); pos = 0;
        }
        void reset() { std::fill(re.begin(), re.end(), 0.0f);
                       std::fill(im.begin(), im.end(), 0.0f); pos = 0; }
        cmplx run(cmplx z) {
            int n = (int)taps.size();
            if (n == 0) return z;
            re[pos] = z.real(); im[pos] = z.imag();
            float ar = 0.0f, ai = 0.0f;
            int p = pos;
            for (int k = 0; k < n; k++) {
                ar += taps[k] * re[p];
                ai += taps[k] * im[p];
                if (--p < 0) p += n;
            }
            if (++pos >= n) pos = 0;
            return cmplx(ar, ai);
        }
    private:
        std::vector<float> taps, re, im;
        int pos = 0;
    };

    // ------------------------------------------------------------------------

    class FeldModem {
    public:
        FeldModem() {
            col_data.assign(2 * MAX_RX_COLUMN_LEN, 0);
            ring.assign((size_t)HELL_IMG_COLS * (2 * MAX_RX_COLUMN_LEN), 255);
            rgb.assign((size_t)HELL_IMG_COLS * (2 * MAX_RX_COLUMN_LEN) * 3, 255);
            configure(MODE_FELDHELL, 1500.0, 20, HELL_AGC_SLOW, false, false);
        }

        // Reconfigure mode / AF / display geometry. Mirrors FLDIGI restart().
        void configure(int modeIdx, double afFreqHz, int rxHeight,
                       int agcMode, bool reverse, bool blackboard) {
            std::lock_guard<std::mutex> lck(mtx);
            mode      = std::clamp(modeIdx, 0, HELL_MODE_COUNT - 1);
            frequency = afFreqHz;
            RxColumnLen = std::clamp(rxHeight, 14, MAX_RX_COLUMN_LEN);
            hellagc   = agcMode;
            this->reverse = reverse;
            this->blackboard = blackboard;

            const HellModeDef& m = HELL_MODES[mode];
            feldcolumnrate = m.feldcolumnrate;
            hell_bandwidth = m.fsk ? m.hell_bandwidth : (FELD_COLUMN_LEN * feldcolumnrate);
            rxpixrate      = RxColumnLen * feldcolumnrate;
            downsampleinc  = rxpixrate / FELD_SAMPLE_RATE;

            if (m.fsk) {
                phi2freq = FELD_SAMPLE_RATE / M_PI / (hell_bandwidth / 2.0);
                filter_bandwidth = 5.0 * std::round(4.0 * hell_bandwidth / 5.0);
            } else {
                phi2freq = 0.0;
                filter_bandwidth = 5.0 * std::round(1.2 * hell_bandwidth / 5.0);
            }

            lpf.design(filter_bandwidth, FELD_SAMPLE_RATE);
            average.setLength((int)std::lround(500.0 * FELD_SAMPLE_RATE / rxpixrate));
            bbfilt.setLength(8);
            resetRx();
        }

        void setAFFreq(double f) { std::lock_guard<std::mutex> lck(mtx); frequency = f; }
        void setReverse(bool r)  { std::lock_guard<std::mutex> lck(mtx); reverse = r; }
        void setBlackboard(bool b){ std::lock_guard<std::mutex> lck(mtx); blackboard = b; }
        void setAgcMode(int a)   { std::lock_guard<std::mutex> lck(mtx); hellagc = a; }
        void setSquelch(bool en, double level) {
            std::lock_guard<std::mutex> lck(mtx); sqlOn = en; sqlLevel = level;
        }

        double getMetric() const { return metric.load(); }
        int    getColumnsReceived() const { return colsReceived.load(); }
        int    getImageWidth()  const { return HELL_IMG_COLS; }
        int    getImageHeight() const { return 2 * RxColumnLen; }
        std::mutex& getImageMutex() { return imgMtx; }

        // Linearise the ring into rgb (newest column on the right) and return it.
        // NOTE: the caller MUST hold getImageMutex() around this call and its use
        // of the returned pointer (same contract as the wefax module). This
        // function does NOT lock internally — doing so would deadlock against the
        // caller's lock since std::mutex is non-recursive.
        const uint8_t* getImageRGB() {
            int h = 2 * RxColumnLen;
            for (int x = 0; x < HELL_IMG_COLS; x++) {
                int src = (wr + x) % HELL_IMG_COLS;     // oldest..newest left..right
                const uint8_t* col = &ring[(size_t)src * (2 * MAX_RX_COLUMN_LEN)];
                for (int y = 0; y < h; y++) {
                    uint8_t v = col[y];
                    size_t o = ((size_t)y * HELL_IMG_COLS + x) * 3;
                    rgb[o] = v; rgb[o + 1] = v; rgb[o + 2] = v;
                }
            }
            return rgb.data();
        }

        void clearImage() {
            std::lock_guard<std::mutex> lck(imgMtx);
            std::fill(ring.begin(), ring.end(), 255);
            std::fill(rgb.begin(), rgb.end(), 255);
            wr = 0; colsReceived = 0;
        }

        // GUI band spectrum (Goertzel bank over the audio passband, smoothed).
        int getBandSpectrum(float* out, int n) {
            std::lock_guard<std::mutex> lck(specMtx);
            int c = std::min(n, SPEC_BINS);
            for (int i = 0; i < c; i++) out[i] = specSm[i];
            return c;
        }
        double getBandFlo() const { return SPEC_FLO; }
        double getBandFhi() const { return SPEC_FHI; }

        // --- main processing: real audio at FELD_SAMPLE_RATE ------------------
        void process(const float* audio, int count) {
            std::lock_guard<std::mutex> lck(mtx);
            for (int i = 0; i < count; i++) {
                float s = audio[i];
                feedSpec(s);

                cmplx z = hilbert.run(s);   // analytic signal
                z = mixer(z);               // NCO down-mix by `frequency`
                z = lpf.run(z);             // baseband low-pass
                if (HELL_MODES[mode].fsk) FSKH_rx(z);
                else                       rx(z);
            }
        }

    private:
        // ---- FLDIGI rx helpers (ported) ------------------------------------
        cmplx mixer(cmplx in) {
            cmplx z(cosf((float)rxphacc), sinf((float)rxphacc));
            z = z * in;
            rxphacc -= 2.0 * M_PI * frequency / FELD_SAMPLE_RATE;
            if (rxphacc < 0) rxphacc += 2.0 * M_PI;
            return z;
        }

        void agcDecay() {
            switch (hellagc) {
                case HELL_AGC_FAST:   agc *= (1.0 - 0.2   / RxColumnLen); break;
                case HELL_AGC_MEDIUM: agc *= (1.0 - 0.075 / RxColumnLen); break;
                case HELL_AGC_SLOW:
                default:              agc *= (1.0 - 0.01  / RxColumnLen);
            }
        }

        // Amplitude (FELDHELL / SLOWHELL / X5 / X9)
        void rx(cmplx z) {
            double x = std::abs(z);
            if (x > peakval) peakval = x;
            double avg = average.run(x);

            rxcounter += downsampleinc;
            if (rxcounter < 1.0) return;
            rxcounter -= 1.0;

            x = peakval; peakval = 0.0;
            if (x > peakhold) peakhold = x;
            else              peakhold *= (1.0 - 0.02 / RxColumnLen);
            int ix = (int)std::clamp(255.0 * x / (peakhold > 0 ? peakhold : 1e-9), 0.0, 255.0);

            if (avg > agc) agc = avg; else agcDecay();
            metric = std::clamp(1000.0 * agc, 0.0, 100.0);

            if (!blackboard) ix = 255 - ix;
            pushPixel(ix);
        }

        // FSK (FSKH245 / FSKH105 / HELL80)
        void FSKH_rx(cmplx z) {
            double f = std::arg(std::conj(prev) * z) * phi2freq;
            prev = z;
            if (reverse) f = -f;          // mark/space swap at the SIGNAL level
                                          // (corrects an inverted / wrong-sideband
                                          //  FSK signal); FSK modes only.
            f = bbfilt.run(f);
            double avg = average.run(std::abs(z));

            rxcounter += downsampleinc;
            if (rxcounter < 1.0) return;
            rxcounter -= 1.0;

            if (avg > agc) agc = avg; else agcDecay();
            metric = std::clamp(1000.0 * agc, 0.0, 100.0);

            double vid = std::clamp(0.5 * (f + 1.0), 0.0, 1.0);
            int ix = (int)(vid * 255.0);
            // Default polarity = dark ink on light paper, consistent with the AM
            // modes; Blackboard inverts the DISPLAY for every mode.
            if (!blackboard) ix = 255 - ix;
            pushPixel(ix);
        }

        // Accumulate a pixel; on a full column emit a 2*RxColumnLen strip.
        void pushPixel(int ix) {
            col_data[col_pointer + RxColumnLen] = ix;
            col_pointer++;
            if (col_pointer >= RxColumnLen) {
                if (!sqlOn || metric > sqlLevel) emitColumn();
                col_pointer = 0;
                for (int i = 0; i < RxColumnLen; i++)
                    col_data[i] = col_data[i + RxColumnLen];
            }
        }

        void emitColumn() {
            std::lock_guard<std::mutex> lck(imgMtx);
            uint8_t* dst = &ring[(size_t)wr * (2 * MAX_RX_COLUMN_LEN)];
            int h = 2 * RxColumnLen;
            // FLDIGI's Raster widget maps col_data[0] to the BOTTOM row and
            // col_data[h-1] to the TOP (vidbuf row = base + (len-1-i)). Replicate
            // that vertical order so glyphs render upright; writing col_data[0] to
            // the top (as before) produced a vertically-flipped image.
            for (int y = 0; y < h; y++)
                dst[y] = (uint8_t)std::clamp(col_data[h - 1 - y], 0, 255);
            wr = (wr + 1) % HELL_IMG_COLS;
            colsReceived++;
        }

        void resetRx() {
            rxcounter = 0.0; peakhold = 0.0; peakval = 0.0; agc = 0.0;
            rxphacc = 0.0; col_pointer = 0; prev = cmplx(0, 0);
            std::fill(col_data.begin(), col_data.end(), 0);
            average.clear(); bbfilt.clear();
            hilbert.reset(); lpf.reset();
        }

        // ---- GUI band spectrum (Goertzel) ----------------------------------
        void feedSpec(float s) {
            specBuf[specPos++] = s;
            if (specPos >= SPEC_ANALYSIS_LEN) {
                computeSpec();
                specPos = 0;
            }
        }
        void computeSpec() {
            float tmp[SPEC_BINS];
            for (int b = 0; b < SPEC_BINS; b++) {
                double f = SPEC_FLO + (SPEC_FHI - SPEC_FLO) * b / (double)(SPEC_BINS - 1);
                double w = 2.0 * M_PI * f / FELD_SAMPLE_RATE;
                double coeff = 2.0 * cos(w);
                double q0 = 0, q1 = 0, q2 = 0;
                for (int i = 0; i < SPEC_ANALYSIS_LEN; i++) {
                    q0 = coeff * q1 - q2 + specBuf[i];
                    q2 = q1; q1 = q0;
                }
                double mag = sqrt(q1 * q1 + q2 * q2 - q1 * q2 * coeff);
                tmp[b] = (float)mag;
            }
            std::lock_guard<std::mutex> lck(specMtx);
            for (int b = 0; b < SPEC_BINS; b++)
                specSm[b] = 0.7f * specSm[b] + 0.3f * tmp[b];
        }

        // ---- state ----------------------------------------------------------
        int    mode = MODE_FELDHELL;
        double frequency = 1500.0;
        int    RxColumnLen = 20;
        int    hellagc = HELL_AGC_SLOW;
        bool   reverse = false;
        bool   blackboard = false;
        bool   sqlOn = true;
        double sqlLevel = 5.0;

        double feldcolumnrate = 17.5;
        double hell_bandwidth = 245.0;
        double rxpixrate = 0.0;
        double downsampleinc = 0.0;
        double filter_bandwidth = 295.0;
        double phi2freq = 0.0;

        double rxcounter = 0.0, rxphacc = 0.0;
        double peakval = 0.0, peakhold = 0.0, agc = 0.0;
        cmplx  prev{0, 0};
        std::atomic<double> metric{0.0};

        HilbertFIR hilbert;
        ComplexLPF lpf;
        MovAvg     average, bbfilt;

        std::vector<int> col_data;
        int  col_pointer = 0;

        // scrolling image ring (grayscale) + linearised RGB snapshot
        std::vector<uint8_t> ring;
        std::vector<uint8_t> rgb;
        int wr = 0;
        std::atomic<int> colsReceived{0};
        std::mutex imgMtx;

        std::mutex mtx;

        // band spectrum
        static constexpr int    SPEC_BINS = 256;
        static constexpr int    SPEC_ANALYSIS_LEN = 1024;
        static constexpr double SPEC_FLO = 0.0;
        static constexpr double SPEC_FHI = 3500.0;
        float  specBuf[SPEC_ANALYSIS_LEN] = {0};
        int    specPos = 0;
        float  specSm[SPEC_BINS] = {0};
        std::mutex specMtx;
    };

}
