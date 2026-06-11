// gps_tracking.cpp -- Per-satellite tracking channel

#include "gps_tracking.h"

#include <algorithm>
#include <cmath>

namespace gps {

static constexpr double TWO_PI = 6.283185307179586476925;

TrackingChannel::TrackingChannel(double sampleRate, float chipSpacing)
    : sampleRate_(sampleRate),
      chipSpacing_(chipSpacing),
      carrierPhase_(0.0),
      codePhaseChips_(0.0),
      carrierFiltMem_(0.0f),
      codeFiltMem_(0.0f),
      sampleInMs_(0)
{
    samplesPerMs_ = (int)std::round(sampleRate_ * CA_CODE_PERIOD);
}

void TrackingChannel::init(int prn, float dopplerHz, int codePhaseSamples) {
    std::lock_guard<std::mutex> l(mu_);

    state_.prn         = prn;
    state_.active      = true;
    state_.carrierFreq = dopplerHz;
    state_.codeFreq    = CA_CHIP_RATE;     // nominal, will be trimmed by the DLL
    state_.cn0_dBHz    = 0.0f;
    state_.msCount     = 0;
    state_.locked      = false;

    generateCaCode(prn, caCode_);

    // The code phase from acquisition is "the integer sample index at which
    // the local replica should *start* to align with the received chunk".
    // Translate this into a fractional chip offset for the code NCO.
    double codePhaseChipsAtSampleZero =
        -((double)codePhaseSamples * CA_CHIP_RATE) / sampleRate_;
    // Wrap to [0, 1023)
    while (codePhaseChipsAtSampleZero < 0.0)
        codePhaseChipsAtSampleZero += CA_CODE_LENGTH;
    while (codePhaseChipsAtSampleZero >= CA_CODE_LENGTH)
        codePhaseChipsAtSampleZero -= CA_CODE_LENGTH;

    codePhaseChips_ = codePhaseChipsAtSampleZero;
    carrierPhase_   = 0.0;
    sampleInMs_     = 0;
    iE_=qE_=iP_=qP_=iL_=qL_=0.0f;
    carrierFiltMem_ = 0.0f;
    codeFiltMem_    = 0.0f;
    cn0_iSquared_.clear();
    cn0_qSquared_.clear();
}

// Sample-by-sample integration over a complete code period.
// After samplesPerMs_ samples we close the loops, update the NCOs and
// publish I_P/Q_P (the on-time correlator output) for navigation decoding.
int TrackingChannel::feed(const std::complex<float>* iq, int n,
                          std::vector<int8_t>* out_navBits,
                          std::vector<int>*    out_msCounts,
                          std::vector<double>* out_codePhases)
{
    std::lock_guard<std::mutex> l(mu_);
    if (!state_.active) return 0;

    int epochsCompleted = 0;
    const double codeRateRatio = state_.codeFreq / sampleRate_;     // chips per IF sample

    for (int i = 0; i < n; i++) {
        // ---- 1. Mix off the carrier (NCO + IQ multiply) -------------------
        float c = std::cos((float)carrierPhase_);
        float s = std::sin((float)carrierPhase_);
        std::complex<float> lo(c, -s);  // exp(-j*phi)
        std::complex<float> baseband = iq[i] * lo;
        float bbI = baseband.real();
        float bbQ = baseband.imag();

        // ---- 2. Code replicas at E / P / L --------------------------------
        // Integer chip indices for early, prompt, late
        double pIdx = codePhaseChips_;
        double eIdx = pIdx - chipSpacing_;
        double lIdx = pIdx + chipSpacing_;

        // Wrap into [0, 1023)
        auto wrap = [](double x) {
            while (x < 0.0)               x += CA_CODE_LENGTH;
            while (x >= CA_CODE_LENGTH)   x -= CA_CODE_LENGTH;
            return x;
        };
        eIdx = wrap(eIdx);
        pIdx = wrap(pIdx);
        lIdx = wrap(lIdx);

        int8_t e = caCode_[(int)eIdx];
        int8_t p = caCode_[(int)pIdx];
        int8_t lt = caCode_[(int)lIdx];

        // ---- 3. Accumulate correlator integrators -------------------------
        iE_ += bbI * e;  qE_ += bbQ * e;
        iP_ += bbI * p;  qP_ += bbQ * p;
        iL_ += bbI * lt; qL_ += bbQ * lt;

        // ---- 4. Advance NCOs ---------------------------------------------
        carrierPhase_   += TWO_PI * state_.carrierFreq / sampleRate_;
        if (carrierPhase_ >  TWO_PI) carrierPhase_ -= TWO_PI;
        if (carrierPhase_ < -TWO_PI) carrierPhase_ += TWO_PI;

        codePhaseChips_ += codeRateRatio;
        if (codePhaseChips_ >= CA_CODE_LENGTH) codePhaseChips_ -= CA_CODE_LENGTH;

        sampleInMs_++;

        // ---- 5. End of code period? Close the loops. ----------------------
        if (sampleInMs_ >= samplesPerMs_) {
            sampleInMs_ = 0;
            state_.msCount++;

            // Code DLL discriminator: non-coherent early-minus-late envelope
            float eMag = std::sqrt(iE_*iE_ + qE_*qE_);
            float lMag = std::sqrt(iL_*iL_ + qL_*qL_);
            float codeErr = (eMag + lMag > 1e-12f) ? (eMag - lMag) / (eMag + lMag) : 0.0f;

            // Carrier Costas discriminator: atan(Q/I), insensitive to ±180°
            float carrErr = (std::fabs(iP_) > 1e-12f) ? std::atan(qP_ / iP_) : 0.0f;

            // Loop filters (simple PI):
            //   y[n] = y[n-1] + (Bn / damping) * err
            // Tuning constants chosen empirically for ~25 Hz carrier loop BW
            // and ~1 Hz code loop BW at 1 kHz update rate.
            float carrierIntGain = carrierLoopBw_ * 0.001f * 4.0f;  // proportional
            float codeIntGain    = codeLoopBw_    * 0.001f * 4.0f;

            // Frequency correction applied to the NCOs
            state_.carrierFreq += carrierIntGain * carrErr;
            state_.codeFreq     = (float)CA_CHIP_RATE + codeIntGain * codeErr * CA_CHIP_RATE;

            // Publish state for the UI
            state_.I_P          = iP_;
            state_.Q_P          = qP_;
            state_.carrierError = carrErr;
            state_.codeError    = codeErr;
            state_.codePhase    = codePhaseChips_;

            // C/N0 estimate (Beaulieu): sliding window of I^2 (signal+noise)
            // and Q^2 (noise only). Window ~ 200 ms.
            const int CN0_WIN = 200;
            cn0_iSquared_.push_back(iP_ * iP_);
            cn0_qSquared_.push_back(qP_ * qP_);
            while ((int)cn0_iSquared_.size() > CN0_WIN) {
                cn0_iSquared_.pop_front();
                cn0_qSquared_.pop_front();
            }
            if (cn0_iSquared_.size() >= 20) {
                double pSignal = 0.0, pNoise = 0.0;
                for (auto v : cn0_iSquared_) pSignal += v;
                for (auto v : cn0_qSquared_) pNoise  += v;
                pSignal /= cn0_iSquared_.size();
                pNoise  /= cn0_qSquared_.size();
                if (pNoise > 1e-20) {
                    // (S - N) / N  per 1 ms = SNR, convert to C/N0 (Hz)
                    double snrLinear = (pSignal - pNoise) / pNoise;
                    if (snrLinear < 1e-6) snrLinear = 1e-6;
                    state_.cn0_dBHz = (float)(10.0 * std::log10(snrLinear) + 30.0);
                                                       // +30 dB = +10*log10(1000 Hz)
                }
            }

            // Lock detector: |I_P| / sqrt(|I_P|^2 + |Q_P|^2) close to 1 means
            // the carrier loop has settled (energy is in I rather than spread)
            if (cn0_iSquared_.size() >= 50 && state_.cn0_dBHz > 32.0f) {
                state_.locked = true;
            }

            // Output the navigation soft symbol: sign of I_P (1 per ms).
            // A bit edge happens every 20 ms; the higher-level navigation
            // decoder integrates 20 of these symbols to get one nav bit.
            // The parallel msCount/codePhase outputs capture the tracker's
            // chip-level state AT this symbol's emission moment, which the
            // higher layer (NavDecoder) keeps for sub-ms pseudorange.
            if (out_navBits) {
                out_navBits->push_back((int8_t)((iP_ >= 0.0f) ? +1 : -1));
            }
            if (out_msCounts)   out_msCounts->push_back(state_.msCount);
            if (out_codePhases) out_codePhases->push_back(codePhaseChips_);

            // Reset accumulators for the next code period
            iE_=qE_=iP_=qP_=iL_=qL_=0.0f;
            epochsCompleted++;
        }
    }

    return epochsCompleted;
}

TrackerState TrackingChannel::getState() {
    std::lock_guard<std::mutex> l(mu_);
    return state_;
}

} // namespace gps
