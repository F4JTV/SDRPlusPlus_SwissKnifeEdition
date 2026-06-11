// gps_tracking.h -- Per-satellite tracking channel
//
// Once a satellite is acquired (we have a coarse Doppler estimate and code
// phase to within ~0.5 chip), it is handed to a tracking channel that runs
// a Delay-Lock Loop (DLL) on the code and a Costas loop (carrier-phase PLL
// that tolerates the 180° flips of the navigation data bits) on the
// carrier.
//
// For every code period (1 ms) we compute three correlators:
//
//     I_E + jQ_E  early    (code replica advanced by chipSpacing chips)
//     I_P + jQ_P  prompt   (on-time replica)
//     I_L + jQ_L  late     (code replica delayed by chipSpacing chips)
//
// The DLL discriminator (non-coherent early-minus-late envelope):
//
//     err_code = (|E| - |L|) / (|E| + |L|)
//
// The Costas discriminator (atan2(Q_P, I_P) reduced to ±pi/2 by atan):
//
//     err_carrier = atan(Q_P / I_P)
//
// Both errors drive 2nd-order loop filters whose outputs trim the local
// carrier NCO and code NCO. C/N0 is estimated from the prompt power using
// the simple "Beaulieu" method.
//
// References:
//   - Kaplan & Hegarty, "Understanding GPS/GNSS: Principles and
//     Applications", 3rd ed., Chapters 5 & 7.
//   - Misra & Enge, "Global Positioning System: Signals, Measurements,
//     and Performance", 2nd ed.

#pragma once

#include <array>
#include <complex>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "gps_ca_code.h"

namespace gps {

struct TrackerState {
    int   prn         = 0;
    bool  active      = false;
    float carrierFreq = 0.0f;     // current carrier NCO frequency (Hz, baseband)
    float codeFreq    = 0.0f;     // current code NCO frequency (chips/s)
    float cn0_dBHz    = 0.0f;     // estimated carrier-to-noise density ratio
    double codePhase  = 0.0;      // chips into the current code period [0..1023)
    int   msCount     = 0;        // number of 1 ms epochs processed since lock
    // For UI / debug
    float I_P = 0.0f, Q_P = 0.0f; // last prompt correlator
    float carrierError = 0.0f;
    float codeError    = 0.0f;
    bool  locked       = false;
};

class TrackingChannel {
public:
    // sampleRate   : IF sample rate
    // chipSpacing  : early/late spacing in chips (typical 0.5)
    TrackingChannel(double sampleRate, float chipSpacing = 0.5f);

    // Initialise the channel from acquisition results.
    void init(int prn, float dopplerHz, int codePhaseSamples);

    // Feed `n` complex samples. The function processes integer ms epochs
    // and updates the loops once per epoch. Returns the number of new
    // 1 ms epochs completed during this call (typically 0 or 1).
    //
    // out_navBits   : optional, receives one soft navigation symbol per epoch
    // out_msCounts  : optional, parallel vector with the tracker's msCount AT
    //                 the moment that symbol was emitted (i.e., AFTER the
    //                 ms increment for that epoch).
    // out_codePhases: optional, parallel vector with codePhase (chips) at
    //                 the start of the next code period (~0..1023, post-wrap).
    int feed(const std::complex<float>* iq, int n,
             std::vector<int8_t>* out_navBits   = nullptr,
             std::vector<int>*    out_msCounts  = nullptr,
             std::vector<double>* out_codePhases = nullptr);

    TrackerState getState();
    void stop()         { std::lock_guard<std::mutex> l(mu_); state_.active = false; }
    bool isActive()     { std::lock_guard<std::mutex> l(mu_); return state_.active; }

private:
    // One full code period correlation, advancing the NCOs sample-by-sample.
    void integrateOneMs(const std::complex<float>* iq);

    double sampleRate_;
    int    samplesPerMs_;
    float  chipSpacing_;

    // Local replica of the active PRN, full 1023 chips at +1 / -1
    std::array<int8_t, CA_CODE_LENGTH> caCode_;

    // NCO state
    double carrierPhase_;             // rad
    double codePhaseChips_;           // current code phase in chips, [0 .. 1023)

    // Loop filter state (2nd order, simplified)
    float carrierFiltMem_;
    float codeFiltMem_;

    // Loop bandwidths (rad/s) -- defaults chosen for moderate dynamics
    float carrierLoopBw_ = 25.0f;
    float codeLoopBw_    = 1.0f;

    // C/N0 averaging buffers (a few hundred ms of history)
    std::deque<float> cn0_iSquared_, cn0_qSquared_;

    TrackerState state_;
    std::mutex   mu_;

    // Accumulators within one ms (reset every code period)
    float iE_=0, qE_=0, iP_=0, qP_=0, iL_=0, qL_=0;

    // Sub-ms sample counter (when this hits samplesPerMs_ we close the loop)
    int sampleInMs_;
};

} // namespace gps
