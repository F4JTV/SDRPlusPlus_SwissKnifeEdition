// gps_decoder.h -- Top-level GPS L1 C/A receiver orchestrator
//
// Glues together the acquisition engine, per-PRN tracking channels and
// navigation message decoders. Receives a stream of complex baseband IQ
// samples from SDR++ at a known sample rate (typically 2.048 MHz) and:
//
//   - keeps a rolling 2 ms acquisition buffer
//   - runs the FFT-based acquisition search in a background thread on a
//     configurable cadence (default every 2 s, all 32 PRNs)
//   - hands acquired satellites off to per-PRN tracking channels which
//     consume the live stream and feed soft symbols into NavDecoder
//     instances
//   - exposes a thread-safe snapshot of all channel states for the GUI

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "gps_acquisition.h"
#include "gps_ca_code.h"
#include "gps_nav.h"
#include "gps_tracking.h"

namespace gps {

struct ChannelSnapshot {
    int    prn          = 0;
    bool   tracking     = false;
    float  dopplerHz    = 0.0f;
    int    codePhase    = 0;
    float  cn0_dBHz     = 0.0f;
    bool   locked       = false;
    int    msTracked    = 0;
    bool   bitSynced    = false;
    int    bitsDecoded  = 0;
    int    subframesDecoded = 0;
    int    lastSubframeId = 0;
    uint32_t lastTow = 0;
};

struct AcqSnapshot {
    int   prn        = 0;
    bool  acquired   = false;
    float dopplerHz  = 0.0f;
    int   codePhase  = 0;
    float peakMetric = 0.0f;
};

class GpsDecoder {
public:
    using SubframeCallback = std::function<void(const SubframeInfo&)>;

    GpsDecoder(double sampleRate, float acquisitionThreshold = 2.5f);
    ~GpsDecoder();

    // Push a block of complex IQ samples. Thread-safe; called by the SDR++
    // sink handler. Non-blocking.
    void pushSamples(const std::complex<float>* iq, int n);

    // Start / stop the background acquisition worker.
    void start();
    void stop();

    // Snapshots for GUI
    std::vector<ChannelSnapshot> getChannelStates();
    std::vector<AcqSnapshot>     getAcquisitionResults();

    // Tuning
    void setAcquisitionThreshold(float t) { acquisition_->setThreshold(t); }
    float getAcquisitionThreshold() const { return acquisition_->getThreshold(); }
    void setAcquisitionPeriodSec(float s) { acqPeriodSec_ = std::max(0.5f, s); }
    float getAcquisitionPeriodSec() const { return acqPeriodSec_; }

    int  getNumActiveChannels();
    void clearChannels();
    void forceAcquireNow()                { reacqRequested_ = true; cv_.notify_all(); }

    void setSubframeCallback(SubframeCallback cb) {
        std::lock_guard<std::mutex> l(cbMu_); subframeCb_ = std::move(cb);
    }

    double getSampleRate() const          { return sampleRate_; }

private:
    void acquisitionThreadFn();

    double sampleRate_;
    int    samplesPerMs_;
    float  acqPeriodSec_ = 2.0f;

    std::unique_ptr<Acquisition> acquisition_;

    // ---- Active tracking channels (key: PRN, value: tracker + decoder) ---
    struct Channel {
        std::unique_ptr<TrackingChannel> tracker;
        std::unique_ptr<NavDecoder>      nav;
        bool removed = false;
    };
    std::array<std::unique_ptr<Channel>, NUM_SATELLITES + 1> channels_; // index 1..32
    std::mutex channelsMu_;

    // ---- Rolling IQ buffer for the acquisition worker --------------------
    // Always holds the most recent ~3 ms of samples (2 ms used for the
    // double-block acquisition, plus a small margin). Tracking channels
    // process the live stream directly; this buffer is purely for the
    // acquisition snapshot.
    std::vector<std::complex<float>> acqBuf_;
    int                              acqBufWritePos_ = 0;
    int                              acqBufLen_      = 0;
    int                              acqBufNeeded_   = 0;
    std::mutex                       acqBufMu_;

    // ---- Acquisition thread ---------------------------------------------
    std::thread             acqThread_;
    std::atomic<bool>       running_{false};
    std::atomic<bool>       reacqRequested_{false};
    std::mutex              cvMu_;
    std::condition_variable cv_;
    std::chrono::steady_clock::time_point lastAcqTime_;

    // ---- Latest acquisition results (for GUI) ---------------------------
    std::vector<AcqSnapshot> lastAcqResults_;
    std::mutex               lastAcqMu_;

    // ---- Subframe callback ----------------------------------------------
    SubframeCallback subframeCb_;
    std::mutex       cbMu_;
};

} // namespace gps
