// gps_decoder.cpp -- Top-level GPS L1 C/A receiver orchestrator

#include "gps_decoder.h"

#include <algorithm>
#include <cstring>

namespace gps {

GpsDecoder::GpsDecoder(double sampleRate, float acquisitionThreshold)
    : sampleRate_(sampleRate)
{
    samplesPerMs_ = (int)std::round(sampleRate_ * CA_CODE_PERIOD);
    acqBufNeeded_ = 3 * samplesPerMs_;
    acqBuf_.assign(acqBufNeeded_, std::complex<float>(0, 0));

    acquisition_ = std::make_unique<Acquisition>(sampleRate_, 5000.0f, 500.0f, acquisitionThreshold);
}

GpsDecoder::~GpsDecoder() {
    stop();
}

void GpsDecoder::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    reacqRequested_ = true;
    acqThread_ = std::thread([this] { acquisitionThreadFn(); });
}

void GpsDecoder::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (acqThread_.joinable()) acqThread_.join();

    std::lock_guard<std::mutex> l(channelsMu_);
    for (auto& c : channels_) c.reset();
}

void GpsDecoder::pushSamples(const std::complex<float>* iq, int n) {
    // 1) Feed tracking channels with the live stream
    {
        std::lock_guard<std::mutex> l(channelsMu_);
        std::vector<int8_t> bits;
        std::vector<int>    bitMs;
        std::vector<double> bitCp;
        bits.reserve(8); bitMs.reserve(8); bitCp.reserve(8);
        for (int p = 1; p <= NUM_SATELLITES; p++) {
            auto& ch = channels_[p];
            if (!ch || !ch->tracker || !ch->tracker->isActive()) continue;
            bits.clear(); bitMs.clear(); bitCp.clear();
            ch->tracker->feed(iq, n, &bits, &bitMs, &bitCp);
            if (!bits.empty() && ch->nav) {
                ch->nav->feed(bits, &bitMs, &bitCp);
            }
        }
    }

    // 2) Append into the rolling acquisition buffer
    {
        std::lock_guard<std::mutex> l(acqBufMu_);
        for (int i = 0; i < n; i++) {
            acqBuf_[acqBufWritePos_] = iq[i];
            acqBufWritePos_ = (acqBufWritePos_ + 1) % acqBufNeeded_;
            if (acqBufLen_ < acqBufNeeded_) acqBufLen_++;
        }
    }
}

void GpsDecoder::acquisitionThreadFn() {
    lastAcqTime_ = std::chrono::steady_clock::now();

    while (running_) {
        // Wait until the next acquisition slot OR a forced re-acq request
        {
            std::unique_lock<std::mutex> l(cvMu_);
            cv_.wait_for(l, std::chrono::milliseconds(200), [&] {
                return !running_ || reacqRequested_.load();
            });
            if (!running_) return;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<float>(now - lastAcqTime_).count();
        if (!reacqRequested_ && elapsed < acqPeriodSec_) continue;

        reacqRequested_ = false;
        lastAcqTime_    = now;

        // Snapshot the acquisition buffer (need 2 ms contiguous)
        std::vector<std::complex<float>> buf;
        {
            std::lock_guard<std::mutex> l(acqBufMu_);
            if (acqBufLen_ < 2 * samplesPerMs_) continue;
            buf.resize(2 * samplesPerMs_);
            // Read from the start: (writePos - acqBufLen_) modulo, but
            // simplest is to read the last 2 ms ending just before the
            // current write head.
            int start = (acqBufWritePos_ - 2 * samplesPerMs_ + acqBufNeeded_) % acqBufNeeded_;
            for (int i = 0; i < 2 * samplesPerMs_; i++) {
                buf[i] = acqBuf_[(start + i) % acqBufNeeded_];
            }
        }

        auto results = acquisition_->searchAll(buf.data());

        // Save the snapshot for the GUI
        {
            std::lock_guard<std::mutex> l(lastAcqMu_);
            lastAcqResults_.clear();
            lastAcqResults_.reserve(results.size());
            for (auto& r : results) {
                AcqSnapshot s;
                s.prn        = r.prn;
                s.acquired   = r.acquired;
                s.dopplerHz  = r.dopplerHz;
                s.codePhase  = r.codePhaseSamples;
                s.peakMetric = r.peakMetric;
                lastAcqResults_.push_back(s);
            }
        }

        // Hand off newly acquired satellites to tracking channels
        {
            std::lock_guard<std::mutex> l(channelsMu_);
            for (auto& r : results) {
                if (!r.acquired) continue;
                auto& ch = channels_[r.prn];
                if (ch && ch->tracker && ch->tracker->isActive()) continue; // already tracking
                ch = std::make_unique<Channel>();
                ch->tracker = std::make_unique<TrackingChannel>(sampleRate_, 0.5f);
                ch->tracker->init(r.prn, r.dopplerHz, r.codePhaseSamples);
                ch->nav     = std::make_unique<NavDecoder>(r.prn);
                int prn = r.prn;
                {
                    std::lock_guard<std::mutex> lc(cbMu_);
                    auto cb = subframeCb_;
                    if (cb) {
                        ch->nav->setSubframeCallback([cb, prn](const SubframeInfo& info) {
                            cb(info);
                        });
                    }
                }
            }
        }
    }
}

std::vector<ChannelSnapshot> GpsDecoder::getChannelStates() {
    std::vector<ChannelSnapshot> out;
    std::lock_guard<std::mutex> l(channelsMu_);
    for (int p = 1; p <= NUM_SATELLITES; p++) {
        auto& ch = channels_[p];
        if (!ch || !ch->tracker || !ch->tracker->isActive()) continue;
        TrackerState ts = ch->tracker->getState();
        ChannelSnapshot s;
        s.prn         = p;
        s.tracking    = true;
        s.dopplerHz   = ts.carrierFreq;
        s.codePhase   = (int)ts.codePhase;
        s.cn0_dBHz    = ts.cn0_dBHz;
        s.locked      = ts.locked;
        s.msTracked   = ts.msCount;
        if (ch->nav) {
            s.bitSynced            = ch->nav->isBitSynced();
            s.bitsDecoded          = ch->nav->getBitsDecoded();
            s.subframesDecoded     = ch->nav->getSubframesDecoded();
            s.fullSubframesDecoded = ch->nav->getFullSubframesDecoded();
            Ephemeris eph = ch->nav->getEphemeris();
            s.eph_sf1 = eph.sf1_received;
            s.eph_sf2 = eph.sf2_received;
            s.eph_sf3 = eph.sf3_received;
            s.eph_consistent = eph.consistent();
        }
        out.push_back(s);
    }
    return out;
}

std::vector<AcqSnapshot> GpsDecoder::getAcquisitionResults() {
    std::lock_guard<std::mutex> l(lastAcqMu_);
    return lastAcqResults_;
}

int GpsDecoder::getNumActiveChannels() {
    std::lock_guard<std::mutex> l(channelsMu_);
    int n = 0;
    for (int p = 1; p <= NUM_SATELLITES; p++) {
        if (channels_[p] && channels_[p]->tracker && channels_[p]->tracker->isActive()) n++;
    }
    return n;
}

void GpsDecoder::clearChannels() {
    std::lock_guard<std::mutex> l(channelsMu_);
    for (auto& c : channels_) c.reset();
}

TimeFix GpsDecoder::getLatestTimeFix() {
    TimeFix best;
    std::lock_guard<std::mutex> l(channelsMu_);
    for (int p = 1; p <= NUM_SATELLITES; p++) {
        auto& ch = channels_[p];
        if (!ch || !ch->tracker || !ch->tracker->isActive() || !ch->nav) continue;
        TrackerState ts = ch->tracker->getState();
        // Reject channels with poor C/N0 — bit errors at low C/N0 will
        // corrupt the HOW word and produce nonsense times.
        if (ts.cn0_dBHz < 30.0f) continue;
        TimeFix fix = ch->nav->getLastTimeFix();
        if (!fix.valid) continue;
        fix.cn0_dBHz = ts.cn0_dBHz;
        if (!best.valid || fix.pc_time > best.pc_time) {
            best = fix;
        }
    }
    return best;
}

std::vector<GpsDecoder::EphemerisStatus> GpsDecoder::getEphemerisStatus() {
    std::vector<EphemerisStatus> out;
    std::lock_guard<std::mutex> l(channelsMu_);
    for (int p = 1; p <= NUM_SATELLITES; p++) {
        auto& ch = channels_[p];
        if (!ch || !ch->tracker || !ch->tracker->isActive() || !ch->nav) continue;
        TrackerState ts = ch->tracker->getState();
        Ephemeris eph = ch->nav->getEphemeris();
        EphemerisStatus s;
        s.prn      = p;
        s.sf1      = eph.sf1_received;
        s.sf2      = eph.sf2_received;
        s.sf3      = eph.sf3_received;
        s.complete = eph.complete();
        s.consistent = eph.consistent();
        s.cn0_dBHz = ts.cn0_dBHz;
        out.push_back(s);
    }
    return out;
}

PvtSolution GpsDecoder::solvePvtFix() {
    // 1) Snapshot all eligible channels' tracker state + ephemeris atomically.
    struct ChannelSnap {
        int           prn;
        Ephemeris     eph;
        TrackerAnchor anchor;
        TrackerState  ts;
    };
    std::vector<ChannelSnap> snaps;
    {
        std::lock_guard<std::mutex> l(channelsMu_);
        for (int p = 1; p <= NUM_SATELLITES; p++) {
            auto& ch = channels_[p];
            if (!ch || !ch->tracker || !ch->tracker->isActive() || !ch->nav) continue;
            TrackerState ts = ch->tracker->getState();
            if (ts.cn0_dBHz < 30.0f) continue;
            Ephemeris eph = ch->nav->getEphemeris();
            if (!eph.consistent()) continue;
            TrackerAnchor anch = ch->nav->getTrackerAnchor();
            if (!anch.valid) continue;
            snaps.push_back({p, eph, anch, ts});
        }
    }
    if (snaps.size() < 4) {
        PvtSolution out; out.used_sats = (int)snaps.size(); return out;
    }

    // 2) Compute each satellite's transmit time at THIS snapshot moment.
    //
    // Using the chip-level relation:
    //   T_tx_now = T_tx_at_anchor + (msCount_now - msCount_anchor) * 1e-3
    //                              + (codePhase_now - codePhase_anchor) / 1.023e6
    // T_tx_at_anchor = anch.gps_tow_seconds (GPS seconds of week).
    // The receiver clock bias and any residual constant offsets fall out
    // of the LS solution into the clock_bias_s unknown.
    //
    // Common receive time T_rx_common: pick any value reasonably close to
    // T_tx + typical signal travel (~75 ms). All channels share it so the
    // LS clock-bias term absorbs whatever constant we pick. We choose
    // max(T_tx) + 0.075 to keep the geometric residual positive.
    std::vector<double> tx_times(snaps.size());
    double tx_max = -1e18;
    for (size_t k = 0; k < snaps.size(); k++) {
        const auto& s = snaps[k];
        double dt_chips = (double)(s.ts.msCount - s.anchor.msCount) * 1023.0
                        + (s.ts.codePhase - s.anchor.codePhase);
        double tx = s.anchor.gps_tow_seconds + dt_chips / 1.023e6;
        // Fold into [0, 604800)
        if (tx < 0)        tx += 7 * 86400.0;
        if (tx >= 604800)  tx -= 7 * 86400.0;
        tx_times[k] = tx;
        if (tx > tx_max) tx_max = tx;
    }
    double t_rx_common = tx_max + 0.075;

    // 3) Build observation set.
    std::vector<PseudorangeObs> obs;
    obs.reserve(snaps.size());
    // Ephemerides need stable storage for the observation pointer chain.
    std::vector<Ephemeris> ephStorage;
    ephStorage.reserve(snaps.size());
    for (size_t k = 0; k < snaps.size(); k++) {
        ephStorage.push_back(snaps[k].eph);
    }
    for (size_t k = 0; k < snaps.size(); k++) {
        double pr = (t_rx_common - tx_times[k]) * SPEED_OF_LIGHT;
        PseudorangeObs o;
        o.prn            = snaps[k].prn;
        o.tx_time_gps_s  = tx_times[k];
        o.pseudorange_m  = pr;
        o.eph            = &ephStorage[k];
        o.cn0_dBHz       = snaps[k].ts.cn0_dBHz;
        obs.push_back(o);
    }

    // 4) Solve. Initial guess at the centre of the Earth (the standard
    //    cold-start choice; LS converges within ~5 iterations).
    PvtSolution sol = solvePvtLeastSquares(obs, t_rx_common, nullptr, 12);
    sol.time = std::chrono::system_clock::now();
    return sol;
}

} // namespace gps
