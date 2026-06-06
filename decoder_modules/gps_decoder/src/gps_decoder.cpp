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
        bits.reserve(8);
        for (int p = 1; p <= NUM_SATELLITES; p++) {
            auto& ch = channels_[p];
            if (!ch || !ch->tracker || !ch->tracker->isActive()) continue;
            bits.clear();
            ch->tracker->feed(iq, n, &bits);
            if (!bits.empty() && ch->nav) {
                ch->nav->feed(bits);
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
            s.bitSynced        = ch->nav->isBitSynced();
            s.bitsDecoded      = ch->nav->getBitsDecoded();
            s.subframesDecoded = ch->nav->getSubframesDecoded();
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

} // namespace gps
