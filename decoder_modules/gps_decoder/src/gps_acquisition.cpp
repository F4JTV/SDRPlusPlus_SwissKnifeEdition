// gps_acquisition.cpp -- FFT-based parallel code-phase GPS acquisition

#include "gps_acquisition.h"
#include "gps_ca_code.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gps {

static constexpr float TWO_PI = 6.28318530717958647692f;

Acquisition::Acquisition(double sampleRate,
                         float  dopplerRangeHz,
                         float  dopplerStepHz,
                         float  threshold)
    : sampleRate_(sampleRate),
      dopplerRangeHz_(dopplerRangeHz),
      dopplerStepHz_(dopplerStepHz),
      threshold_(threshold)
{
    samplesPerMs_ = (int)std::round(sampleRate_ * CA_CODE_PERIOD);

    // FFTW plans -- single-precision, in-place would be possible but using
    // separate input/output buffers keeps the code clear.
    fft_in_  = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * samplesPerMs_);
    fft_out_ = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * samplesPerMs_);
    fft_fwd_ = fftwf_plan_dft_1d(samplesPerMs_, fft_in_, fft_out_,
                                  FFTW_FORWARD,  FFTW_ESTIMATE);
    fft_inv_ = fftwf_plan_dft_1d(samplesPerMs_, fft_in_, fft_out_,
                                  FFTW_BACKWARD, FFTW_ESTIMATE);

    mixed_.resize(samplesPerMs_);
    magSquared_.resize(samplesPerMs_);

    rebuildDopplerGrid();
    buildPrnFftCache();
}

Acquisition::~Acquisition() {
    if (fft_fwd_) fftwf_destroy_plan(fft_fwd_);
    if (fft_inv_) fftwf_destroy_plan(fft_inv_);
    if (fft_in_)  fftwf_free(fft_in_);
    if (fft_out_) fftwf_free(fft_out_);
}

void Acquisition::rebuildDopplerGrid() {
    dopplerBins_.clear();
    int nBinsHalf = (int)std::ceil(dopplerRangeHz_ / dopplerStepHz_);
    for (int k = -nBinsHalf; k <= nBinsHalf; k++) {
        dopplerBins_.push_back(k * dopplerStepHz_);
    }
}

void Acquisition::buildPrnFftCache() {
    prnFft_.assign(NUM_SATELLITES, std::vector<std::complex<float>>(samplesPerMs_));

    std::vector<float> replica;
    for (int p = 1; p <= NUM_SATELLITES; p++) {
        generateCaCodeResampled(p, samplesPerMs_, replica);

        // FFT the replica (treat real sequence as complex with Im = 0)
        for (int n = 0; n < samplesPerMs_; n++) {
            fft_in_[n][0] = replica[n];
            fft_in_[n][1] = 0.0f;
        }
        fftwf_execute(fft_fwd_);

        auto& dst = prnFft_[p - 1];
        for (int n = 0; n < samplesPerMs_; n++) {
            // Store conjugate of the FFT to save a conjugation per ms later
            dst[n] = std::complex<float>(fft_out_[n][0], -fft_out_[n][1]);
        }
    }
}

// Search a single 1 ms IQ chunk for a given PRN, returning the best
// (Doppler, code-phase) and the peak/mean ratio.
static void searchOneMs(const std::complex<float>* iq,
                        int samplesPerMs,
                        const std::vector<float>& dopplerBins,
                        const std::vector<std::complex<float>>& prnFftConj,
                        double sampleRate,
                        fftwf_plan fwd, fftwf_plan inv,
                        fftwf_complex* fftIn, fftwf_complex* fftOut,
                        std::vector<std::complex<float>>& mixed,
                        std::vector<float>& magSquared,
                        float& bestDoppler, int& bestCodePhase,
                        float& bestPeak, float& meanAtBest)
{
    bestDoppler   = 0.0f;
    bestCodePhase = 0;
    bestPeak      = 0.0f;
    meanAtBest    = 1e-9f;

    const float Ts = 1.0f / (float)sampleRate;

    for (float fd : dopplerBins) {
        // Mix the IF samples by exp(-j*2*pi*fd*n*Ts)
        const float dphi = -TWO_PI * fd * Ts;
        float phase = 0.0f;
        for (int n = 0; n < samplesPerMs; n++) {
            float c = std::cos(phase);
            float s = std::sin(phase);
            std::complex<float> rot(c, s);
            mixed[n] = iq[n] * rot;
            phase += dphi;
            if (phase >  TWO_PI) phase -= TWO_PI;
            if (phase < -TWO_PI) phase += TWO_PI;
        }

        // FFT of the mixed samples
        for (int n = 0; n < samplesPerMs; n++) {
            fftIn[n][0] = mixed[n].real();
            fftIn[n][1] = mixed[n].imag();
        }
        fftwf_execute(fwd);

        // Element-wise multiply X * conj(C). prnFftConj already holds conj.
        for (int k = 0; k < samplesPerMs; k++) {
            std::complex<float> X(fftOut[k][0], fftOut[k][1]);
            std::complex<float> Y = X * prnFftConj[k];
            fftIn[k][0] = Y.real();
            fftIn[k][1] = Y.imag();
        }

        // IFFT -> correlation in the time domain
        fftwf_execute(inv);

        // Find the main peak.
        float peak    = 0.0f;
        int   peakIdx = 0;
        for (int n = 0; n < samplesPerMs; n++) {
            float re = fftOut[n][0];
            float im = fftOut[n][1];
            float m  = re*re + im*im;
            magSquared[n] = m;
            if (m > peak) { peak = m; peakIdx = n; }
        }

        // Find the secondary peak, excluding a window of ±2 chips around
        // the main peak. This is the GNSS-SDR canonical acquisition
        // metric: a real satellite produces a single sharp lobe; noise has
        // many similar lobes scattered everywhere. Peak / secondary-peak
        // separates the two cleanly with a low (1.5..3.0) threshold.
        int exclSamples = (int)std::ceil((double)samplesPerMs / CA_CODE_LENGTH * 2.0);
        float secondPeak = 0.0f;
        for (int n = 0; n < samplesPerMs; n++) {
            int dist = n - peakIdx;
            if (dist >  samplesPerMs / 2) dist -= samplesPerMs;
            if (dist < -samplesPerMs / 2) dist += samplesPerMs;
            if (std::abs(dist) <= exclSamples) continue;
            if (magSquared[n] > secondPeak) secondPeak = magSquared[n];
        }

        if (peak > bestPeak) {
            bestPeak       = peak;
            bestDoppler    = fd;
            bestCodePhase  = peakIdx;
            meanAtBest     = (secondPeak > 0.0f) ? secondPeak : 1e-9f;
        }
    }
}

AcquisitionResult Acquisition::searchPrn(int prn, const std::complex<float>* iq) {
    std::lock_guard<std::mutex> l(mu_);

    AcquisitionResult res;
    res.prn = prn;
    if (prn < 1 || prn > NUM_SATELLITES) return res;

    float bestDoppler1 = 0.0f, bestDoppler2 = 0.0f;
    int   bestPhase1   = 0,    bestPhase2   = 0;
    float bestPeak1    = 0.0f, bestPeak2    = 0.0f;
    float mean1        = 1e-9f, mean2       = 1e-9f;

    const auto& prnFftConj = prnFft_[prn - 1];

    // Search two consecutive 1 ms blocks and keep the strongest result.
    // This is the standard trick to dodge the 20 ms nav-bit edges.
    searchOneMs(iq,                samplesPerMs_, dopplerBins_, prnFftConj,
                sampleRate_, fft_fwd_, fft_inv_, fft_in_, fft_out_,
                mixed_, magSquared_,
                bestDoppler1, bestPhase1, bestPeak1, mean1);

    searchOneMs(iq + samplesPerMs_, samplesPerMs_, dopplerBins_, prnFftConj,
                sampleRate_, fft_fwd_, fft_inv_, fft_in_, fft_out_,
                mixed_, magSquared_,
                bestDoppler2, bestPhase2, bestPeak2, mean2);

    float peak, mean, doppler;
    int   phase;
    if (bestPeak1 / mean1 >= bestPeak2 / mean2) {
        peak = bestPeak1; mean = mean1; doppler = bestDoppler1; phase = bestPhase1;
    } else {
        peak = bestPeak2; mean = mean2; doppler = bestDoppler2; phase = bestPhase2;
    }

    float ratio = (mean > 0.0f) ? (peak / mean) : 0.0f;
    res.peakValue        = peak;
    res.peakMetric       = ratio;
    res.dopplerHz        = doppler;
    res.codePhaseSamples = phase;
    res.acquired         = (ratio >= threshold_);
    return res;
}

std::vector<AcquisitionResult> Acquisition::searchAll(const std::complex<float>* iq) {
    std::vector<AcquisitionResult> out;
    out.reserve(NUM_SATELLITES);
    for (int p = 1; p <= NUM_SATELLITES; p++) {
        out.push_back(searchPrn(p, iq));
    }
    return out;
}

} // namespace gps
