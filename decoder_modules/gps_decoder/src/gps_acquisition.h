// gps_acquisition.h -- FFT-based parallel code-phase GPS acquisition
//
// The classic "parallel code-phase search" algorithm (Borre, Akos et al.,
// "A Software-Defined GPS and Galileo Receiver", 2007):
//
//   1. Mix 1 ms of the incoming complex baseband by a candidate carrier
//      frequency: x_d[n] = x[n] * exp(-j*2*pi*f_d*n*Ts)
//   2. FFT the mixed samples -> X_d[k]
//   3. Multiply element-wise by the conjugate of the FFT of the local
//      C/A code replica, FFT(c[n]).
//   4. IFFT the product. The magnitude squared peak indicates the code
//      delay; its location is the code phase, its value is the
//      correlation score.
//
// The search loops over candidate Doppler bins f_d and over all PRNs of
// interest. A satellite is declared acquired when its peak exceeds the
// noise floor by a configurable margin (peak / mean ratio).
//
// To make the search robust against navigation bit transitions every
// 20 ms (which can flip the correlation polarity halfway through the
// integration), we acquire on N consecutive 1 ms blocks and keep the
// best result. Two consecutive ms are sufficient in practice: at least
// one is guaranteed to contain no bit transition.

#pragma once

#include <complex>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <fftw3.h>

namespace gps {

struct AcquisitionResult {
    int   prn         = 0;       // satellite number (1..32) or 0 if invalid
    bool  acquired    = false;   // true if peak exceeded threshold
    float dopplerHz   = 0.0f;    // estimated carrier Doppler shift
    int   codePhaseSamples = 0;  // best code phase in IF samples [0 .. samplesPerMs-1]
    float peakMetric  = 0.0f;    // peak-to-mean ratio (rough C/N0 proxy)
    float peakValue   = 0.0f;    // raw |correlation|^2 at peak
};

class Acquisition {
public:
    // sampleRate     : IF sample rate (e.g. 2.048e6)
    // dopplerRangeHz : ±this much around 0 Hz Doppler (e.g. 5000)
    // dopplerStepHz  : grid step (e.g. 500). Finer = slower but more accurate.
    // threshold      : peak / mean ratio above which a satellite is declared
    //                  acquired. Typical: 2.5 .. 4.0.
    Acquisition(double sampleRate,
                float  dopplerRangeHz = 5000.0f,
                float  dopplerStepHz  = 500.0f,
                float  threshold      = 2.5f);

    ~Acquisition();

    // Run a single PRN search over the supplied IQ buffer. The buffer must
    // contain at least 2 * samplesPerMs() complex samples (we use two 1 ms
    // chunks to avoid nav-bit edge issues).
    AcquisitionResult searchPrn(int prn, const std::complex<float>* iq);

    // Convenience: run searchPrn for every PRN in [1..32] sequentially.
    // Returns one AcquisitionResult per PRN.
    std::vector<AcquisitionResult> searchAll(const std::complex<float>* iq);

    // Tuning knobs (thread-safe via internal mutex)
    void setThreshold(float t)            { std::lock_guard<std::mutex> l(mu_); threshold_ = t; }
    void setDopplerRange(float rangeHz)   { std::lock_guard<std::mutex> l(mu_); dopplerRangeHz_ = rangeHz; rebuildDopplerGrid(); }
    void setDopplerStep(float stepHz)     { std::lock_guard<std::mutex> l(mu_); dopplerStepHz_  = stepHz;  rebuildDopplerGrid(); }

    float getThreshold()    const         { return threshold_; }
    float getDopplerRange() const         { return dopplerRangeHz_; }
    float getDopplerStep()  const         { return dopplerStepHz_; }
    int   getSamplesPerMs() const         { return samplesPerMs_; }
    double getSampleRate() const          { return sampleRate_; }

private:
    void rebuildDopplerGrid();
    void buildPrnFftCache();

    double sampleRate_;
    int    samplesPerMs_;          // samples per 1 ms code period
    float  dopplerRangeHz_;
    float  dopplerStepHz_;
    float  threshold_;

    std::vector<float> dopplerBins_;

    // FFTW plans (re-used across all calls to amortize plan cost)
    fftwf_plan   fft_fwd_  = nullptr;
    fftwf_plan   fft_inv_  = nullptr;
    fftwf_complex* fft_in_  = nullptr;
    fftwf_complex* fft_out_ = nullptr;

    // Pre-computed FFT of each PRN's local replica (one entry per PRN 1..32)
    std::vector<std::vector<std::complex<float>>> prnFft_;

    // Scratch buffers
    std::vector<std::complex<float>> mixed_;
    std::vector<float>               magSquared_;

    std::mutex mu_;
};

} // namespace gps
