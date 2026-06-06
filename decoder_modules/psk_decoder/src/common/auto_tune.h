#pragma once
#include <dsp/processor.h>
#include <dsp/stream.h>
#include <cmath>
#include <cstring>
#include <atomic>
#include <mutex>
#include <algorithm>

// ---------------------------------------------------------------------------
//  AutoTuner
//
//  Pass-through DSP block on the SSB/NFM audio stream. When the operator
//  clicks "Auto AF" the block accumulates a power spectrum estimate using
//  Welch's method (averaging power spectra over short segments) across the
//  full SSB passband. The candidate frequency with the highest accumulated
//  power - refined by parabolic interpolation on its two neighbours for
//  sub-bin precision - is the centre frequency of the PSK signal.
//
//  Goertzel filters are used instead of FFT because we only need ~221 bins
//  over a fixed grid, and Goertzel avoids the FFTW dependency.
//
//  Segment length L = SR / F_STEP makes each Goertzel bin's bandwidth match
//  the candidate spacing, so signals are not lost between grid points.
//
//  Inspired by the MFSK module's auto-acquisition (which sweeps tone-0
//  candidates with a confidence x entropy metric). PSK only needs the peak
//  position, so the entropy term is not used here.
//
//  When idle the block costs only a memcpy per pull, so it's safe to leave
//  permanently in the chain.
// ---------------------------------------------------------------------------

namespace fldigi {

    class AutoTuner : public dsp::Processor<float, float> {
        using base_type = dsp::Processor<float, float>;
    public:
        // Candidate grid covering the SSB audio passband
        static constexpr double F_MIN     = 500.0;     // Hz
        static constexpr double F_MAX     = 2700.0;    // Hz
        static constexpr double F_STEP    = 10.0;      // Hz
        static constexpr int    N_CAND    = (int)((F_MAX - F_MIN) / F_STEP) + 1;  // 221
        static constexpr int    N_SEGMENTS = 25;       // segments to average
        // SEG_LEN is set at init() from sampleRate / F_STEP so the Goertzel
        // bin bandwidth equals F_STEP (no off-grid losses).

        AutoTuner() = default;

        void init(dsp::stream<float>* in, double audioSampleRate) {
            sampleRate = audioSampleRate;
            segLen     = (int)(sampleRate / F_STEP);   // e.g. 2400 at 24 kHz
            targetSamples = segLen * N_SEGMENTS;       // ~60000 = 2.5 s
            for (int i = 0; i < N_CAND; i++) {
                double f = F_MIN + i * F_STEP;
                coef[i] = 2.0 * std::cos(2.0 * M_PI * f / audioSampleRate);
            }
            clearState();
            base_type::init(in);
        }

        // Request a new scan. Idempotent; clears any previous result.
        void beginScan() {
            std::lock_guard<std::mutex> lck(mtx);
            clearState();
            running = true;
        }

        // Abort any running scan and clear state.
        void cancelScan() {
            std::lock_guard<std::mutex> lck(mtx);
            clearState();
        }

        bool isScanning() const { return running.load(); }

        // 0..1 progress. Only meaningful while isScanning() is true.
        float progress() const {
            int done = samplesDone.load();
            if (targetSamples <= 0) return 0.0f;
            float p = (float)done / (float)targetSamples;
            return std::clamp(p, 0.0f, 1.0f);
        }

        bool isReady() const {
            return running.load() && (samplesDone.load() >= targetSamples);
        }

        // Returns the best AF freq (Hz) and clears state. Returns -1 if no
        // scan completed yet.
        float fetchResult() {
            std::lock_guard<std::mutex> lck(mtx);
            if (!running.load() || samplesDone.load() < targetSamples) {
                return -1.0f;
            }

            // Find peak in accumulated power spectrum
            double maxE = -1.0;
            int    maxI = 0;
            for (int i = 0; i < N_CAND; i++) {
                if (accumPower[i] > maxE) { maxE = accumPower[i]; maxI = i; }
            }

            // Energy-weighted centroid over the contiguous region around the
            // peak where power stays above half the max. For a sharp single
            // peak this gives the peak position with sub-bin precision (and
            // is symmetric, so no flat-top bias). For dual-peak signatures
            // such as BPSK idle (two sidebands at +/- baud/2 with a valley
            // at the carrier), the centroid is the midpoint between the
            // two sidebands i.e. the actual carrier frequency.
            double half = 0.5 * maxE;
            int    lo   = maxI;
            int    hi   = maxI;
            while (lo > 0           && accumPower[lo - 1] > half) { lo--; }
            while (hi < N_CAND - 1  && accumPower[hi + 1] > half) { hi++; }
            // For a dual-peak case the centre bin sits in the valley below
            // half-max, breaking contiguity. Extend the window symmetrically
            // around maxI up to the first bin that's below 0.25*max, so the
            // two sidebands are both captured.
            double quarter = 0.25 * maxE;
            int    radius  = std::max(maxI - lo, hi - maxI);
            int    rlo = std::max(0,            maxI - radius - 2);
            int    rhi = std::min(N_CAND - 1,   maxI + radius + 2);
            // Stop expansion when we drop well below quarter-max on each side
            while (rlo > 0          && accumPower[rlo - 1] > quarter) { rlo--; }
            while (rhi < N_CAND - 1 && accumPower[rhi + 1] > quarter) { rhi++; }

            double sumW = 0.0;
            double sumI = 0.0;
            for (int i = rlo; i <= rhi; i++) {
                double w = accumPower[i];
                if (w < quarter) continue;       // ignore noise floor in centroid
                sumW += w;
                sumI += i * w;
            }
            double bestI = (sumW > 0.0) ? (sumI / sumW) : (double)maxI;
            clearState();
            return (float)(F_MIN + bestI * F_STEP);
        }

        int run() override {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }

            // Always pass audio through unchanged
            std::memcpy(base_type::out.writeBuf,
                        base_type::_in->readBuf,
                        count * sizeof(float));

            // Always update the smoothed display spectrum (also accumulates
            // into accumPower if a scan is in progress).
            accumulate(base_type::_in->readBuf, count);
            if (running.load()) {
                int alreadyDone = samplesDone.load();
                int remaining   = targetSamples - alreadyDone;
                if (remaining > 0) {
                    int n = (count < remaining) ? count : remaining;
                    samplesDone.store(alreadyDone + n);
                }
            }

            base_type::_in->flush();
            if (!base_type::out.swap(count)) { return -1; }
            return count;
        }

        // ---- Live band spectrum (for the GUI band view) --------------------
        // Returns the smoothed magnitude spectrum (length min(n, N_CAND)).
        int getBandSpectrum(float* out, int n) const {
            std::lock_guard<std::mutex> lck(specMtx);
            int c = (n < N_CAND) ? n : N_CAND;
            for (int i = 0; i < c; i++) { out[i] = displaySpec[i]; }
            return c;
        }
        double getBandFlo() const { return F_MIN; }
        double getBandFhi() const { return F_MAX; }

    private:
        void clearState() {
            for (int i = 0; i < N_CAND; i++) {
                s1[i] = 0.0;
                s2[i] = 0.0;
                accumPower[i] = 0.0;
            }
            samplesInSeg = 0;
            samplesDone.store(0);
            running.store(false);
            // displaySpec[] is intentionally NOT cleared: it lives across
            // scans so the GUI band view is always populated.
        }

        // Welch-style segmented Goertzel: for each L=segLen samples,
        // compute power per candidate; the per-segment spectrum is smoothed
        // into displaySpec[] for the GUI and (if a scan is active) also
        // added to accumPower for the auto-tune decision.
        void accumulate(const float* x, int n) {
            std::lock_guard<std::mutex> lckSpec(specMtx);
            std::lock_guard<std::mutex> lck(mtx);
            for (int k = 0; k < n; k++) {
                float xk = x[k];
                for (int i = 0; i < N_CAND; i++) {
                    double s0 = (double)xk + coef[i] * s1[i] - s2[i];
                    s2[i] = s1[i];
                    s1[i] = s0;
                }
                samplesInSeg++;
                if (samplesInSeg >= segLen) {
                    bool scanning = running.load();
                    for (int i = 0; i < N_CAND; i++) {
                        double e = s1[i]*s1[i] + s2[i]*s2[i] - coef[i]*s1[i]*s2[i];
                        if (e < 0.0) { e = 0.0; }
                        // Smoothed magnitude for the GUI
                        float mag = (float)std::sqrt(e);
                        displaySpec[i] = 0.7f * displaySpec[i] + 0.3f * mag;
                        // Accumulator for the scan (only when running)
                        if (scanning) { accumPower[i] += e; }
                        s1[i] = 0.0;
                        s2[i] = 0.0;
                    }
                    samplesInSeg = 0;
                }
            }
        }

        std::mutex          mtx;
        mutable std::mutex  specMtx;
        std::atomic<bool>   running{false};
        std::atomic<int>    samplesDone{0};

        double sampleRate    = 24000.0;
        int    segLen        = 2400;
        int    targetSamples = 60000;
        int    samplesInSeg  = 0;

        double coef[N_CAND];
        double s1[N_CAND];
        double s2[N_CAND];
        double accumPower[N_CAND];
        float  displaySpec[N_CAND] = {};  // smoothed live magnitude spectrum
    };

}
