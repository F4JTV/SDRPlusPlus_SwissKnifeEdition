/**
 * @file tracker_engine.h
 * @brief Satellite tracking engine wrapping libpredict (SGP4/SDP4).
 *
 * Runs a background thread that, for the selected satellite and observer (QTH):
 *   - propagates the orbit (predict_orbit),
 *   - observes it from the QTH (predict_observe_orbit) -> az/el/range/range-rate,
 *   - computes the Doppler shift on the configured downlink (predict_doppler_shift),
 *   - computes next AOS / LOS / max-elevation (predict_next_aos/los, scan),
 *   - when Doppler correction is enabled and the satellite is above the minimum
 *     elevation, calls a user tuning callback with the corrected downlink so the
 *     module can retune the SDR (via tuner::tune), keeping the bird centred.
 *
 * libpredict's prediction calls take const observer/elements and write into
 * caller structs, so a single mutex guarding pointer swaps and the snapshot is
 * enough for thread safety.
 *
 * libpredict is GPLv2+; see src/predict/.
 */
#pragma once

extern "C" {
#include "predict/predict.h"
}

#include "tle_manager.h"

#include <atomic>
#include <cmath>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace sattrack {

#ifndef ST_DEG2RAD
#define ST_DEG2RAD (M_PI / 180.0)
#define ST_RAD2DEG (180.0 / M_PI)
#endif

struct TrackSnapshot {
    bool        haveTLE = false;
    bool        valid = false;        // a propagation has been computed
    std::string satName;
    int         norad = -1;

    double az = 0.0;          // deg
    double el = 0.0;          // deg
    double range = 0.0;       // km
    double rangeRate = 0.0;   // km/s (negative = approaching)
    double dopplerHz = 0.0;   // Doppler on downlink (Hz, add to downlink)
    double correctedDownlink = 0.0; // Hz
    double uplinkDopplerHz = 0.0;   // Doppler on uplink (Hz)
    double correctedUplink = 0.0;   // Hz

    double subLat = 0.0;      // deg, sub-satellite point
    double subLon = 0.0;      // deg
    double altKm = 0.0;
    double footprintKm = 0.0;
    bool   eclipsed = false;
    bool   decayed = false;
    bool   aboveHorizon = false;

    int64_t nextAOS = 0;      // unix time, 0 if unknown / currently in pass start
    int64_t nextLOS = 0;      // unix time
    double  maxEl = 0.0;      // deg, max elevation of the upcoming/current pass
};

class TrackerEngine {
public:
    using TuneFn = std::function<void(double correctedDownlinkHz)>;
    // Fired from the background thread after every update cycle, regardless of
    // whether the module's GUI is visible. Used to push map/TCP data so it keeps
    // flowing when the module panel is collapsed.
    using UpdateFn = std::function<void(const TrackSnapshot&)>;

    TrackerEngine() {
        observer = predict_create_observer("QTH", 0.0, 0.0, 0.0);
        running = true;
        worker = std::thread(&TrackerEngine::run, this);
    }

    ~TrackerEngine() {
        running = false;
        if (worker.joinable()) { worker.join(); }
        std::lock_guard<std::mutex> lck(mtx);
        if (orbit) { predict_destroy_orbital_elements(orbit); orbit = nullptr; }
        if (observer) { predict_destroy_observer(observer); observer = nullptr; }
    }

    void setObserver(double latDeg, double lonDeg, double altM) {
        std::lock_guard<std::mutex> lck(mtx);
        qthLat = latDeg; qthLon = lonDeg; qthAlt = altM;
        if (observer) { predict_destroy_observer(observer); }
        observer = predict_create_observer("QTH", latDeg * ST_DEG2RAD, lonDeg * ST_DEG2RAD, altM);
        needsPassUpdate = true;
    }

    void setTLE(const TLE& tle) {
        std::lock_guard<std::mutex> lck(mtx);
        if (orbit) { predict_destroy_orbital_elements(orbit); orbit = nullptr; }
        orbit = predict_parse_tle(tle.line1.c_str(), tle.line2.c_str());
        curName = tle.name;
        curNorad = tle.norad;
        haveTLE = (orbit != nullptr);
        needsPassUpdate = true;
    }

    void clearTLE() {
        std::lock_guard<std::mutex> lck(mtx);
        if (orbit) { predict_destroy_orbital_elements(orbit); orbit = nullptr; }
        haveTLE = false;
        curName.clear();
        curNorad = -1;
    }

    void setDownlink(double hz)    { downlinkHz = hz; }
    void setUplink(double hz)      { uplinkHz = hz; }
    void setTuneCallback(TuneFn f) { std::lock_guard<std::mutex> lck(cbMtx); tuneCb = std::move(f); }
    void setUpdateCallback(UpdateFn f) { std::lock_guard<std::mutex> lck(cbMtx); updateCb = std::move(f); }
    // Stop the worker thread. Safe to call more than once; the destructor also
    // stops it. Call this before tearing down anything the update callback uses.
    void stop() {
        running = false;
        if (worker.joinable()) { worker.join(); }
    }
    void setCorrectionEnabled(bool e) { correctionEnabled = e; }
    void setMinElevation(double d) { minElDeg = d; }
    void setUpdateIntervalMs(int m){ updateMs = (m < 100) ? 100 : m; }
    void setStepHz(double hz)      { stepHz = (hz < 0) ? 0 : hz; }
    // simulated time offset (for testing future passes); 0 = real time
    void setTimeOffset(double s)   { timeOffset = s; }

    TrackSnapshot snapshot() {
        std::lock_guard<std::mutex> lck(snapMtx);
        return snap;
    }

private:
    void run() {
        double lastTuned = -1e18;
        while (running.load()) {
            int sleepMs = updateMs.load();
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));

            TrackSnapshot s;
            bool doTune = false;
            double tuneFreq = 0.0;

            {
                std::lock_guard<std::mutex> lck(mtx);
                s.haveTLE = haveTLE;
                s.satName = curName;
                s.norad = curNorad;

                if (!haveTLE || !orbit || !observer) {
                    storeSnapshot(s);
                    fireUpdate(s);
                    continue;
                }

                double t = (double)std::time(nullptr) + timeOffset.load();
                predict_julian_date_t jd = predict_to_julian_double(t);

                struct predict_position pos;
                struct predict_observation obs;
                predict_orbit(orbit, &pos, jd);
                predict_observe_orbit(observer, &pos, &obs);

                s.valid = true;
                s.az = obs.azimuth * ST_RAD2DEG;
                s.el = obs.elevation * ST_RAD2DEG;
                s.range = obs.range;
                s.rangeRate = obs.range_rate;
                s.subLat = pos.latitude * ST_RAD2DEG;
                s.subLon = pos.longitude * ST_RAD2DEG;
                s.altKm = pos.altitude;
                s.footprintKm = pos.footprint;
                s.eclipsed = (pos.eclipsed != 0);
                s.decayed = pos.decayed;
                s.aboveHorizon = (s.el > 0.0);

                double dl = downlinkHz.load();
                double ul = uplinkHz.load();
                if (dl > 0.0) {
                    s.dopplerHz = predict_doppler_shift(&obs, dl);
                    s.correctedDownlink = dl + s.dopplerHz;
                }
                if (ul > 0.0) {
                    // Uplink Doppler is the opposite correction to apply at the TX.
                    s.uplinkDopplerHz = predict_doppler_shift(&obs, ul);
                    s.correctedUplink = ul - s.uplinkDopplerHz;
                }

                // Pass predictions (recomputed when stale or on demand).
                if (needsPassUpdate || t > cachedLosUnix || cachedAosUnix == 0) {
                    computePass(t);
                    needsPassUpdate = false;
                }
                s.nextAOS = (int64_t)cachedAosUnix;
                s.nextLOS = (int64_t)cachedLosUnix;
                s.maxEl = cachedMaxEl;

                // Decide whether to retune.
                if (correctionEnabled.load() && dl > 0.0 && s.el >= minElDeg.load()) {
                    double target = s.correctedDownlink;
                    if (std::fabs(target - lastTuned) >= stepHz.load()) {
                        doTune = true;
                        tuneFreq = target;
                        lastTuned = target;
                    }
                }
                else {
                    lastTuned = -1e18; // reset so we retune immediately on next AOS
                }
            }

            storeSnapshot(s);
            fireUpdate(s);

            if (doTune) {
                std::lock_guard<std::mutex> lck(cbMtx);
                if (tuneCb) { tuneCb(tuneFreq); }
            }
        }
    }

    // mtx held by caller
    void computePass(double t) {
        cachedAosUnix = 0;
        cachedLosUnix = 0;
        cachedMaxEl = 0.0;

        if (!orbit || !observer) { return; }

        // Geostationary / always-up or never-up birds: predict_next_aos loops.
        if (predict_is_geosynchronous(orbit)) { return; }

        // Skip satellites that never rise above the horizon at this QTH,
        // otherwise predict_next_aos can iterate without converging.
        if (!predict_aos_happens(orbit, qthLat * ST_DEG2RAD)) { return; }

        predict_julian_date_t now = predict_to_julian_double(t);

        struct predict_observation aos = predict_next_aos(observer, orbit, now);
        struct predict_observation los = predict_next_los(observer, orbit, now);

        cachedAosUnix = (double)predict_from_julian(aos.time);
        cachedLosUnix = (double)predict_from_julian(los.time);

        // If we are mid-pass, AOS is in the past; clamp so the UI shows "now".
        if (cachedAosUnix > cachedLosUnix) {
            cachedAosUnix = t; // already acquired
        }

        struct predict_observation maxobs = predict_at_max_elevation(observer, orbit, now);
        cachedMaxEl = maxobs.elevation * ST_RAD2DEG;
    }

    void storeSnapshot(const TrackSnapshot& s) {
        std::lock_guard<std::mutex> lck(snapMtx);
        snap = s;
    }

    // libpredict state (guarded by mtx)
    predict_observer_t*         observer = nullptr;
    predict_orbital_elements_t* orbit = nullptr;
    std::string curName;
    int         curNorad = -1;
    bool        haveTLE = false;
    double      qthLat = 0, qthLon = 0, qthAlt = 0;
    bool        needsPassUpdate = false;
    double      cachedAosUnix = 0, cachedLosUnix = 0, cachedMaxEl = 0;
    std::mutex  mtx;

    // config (atomics, no lock needed)
    std::atomic<double> downlinkHz{ 0.0 };
    std::atomic<double> uplinkHz{ 0.0 };
    std::atomic<bool>   correctionEnabled{ false };
    std::atomic<double> minElDeg{ 0.0 };
    std::atomic<int>    updateMs{ 1000 };
    std::atomic<double> stepHz{ 10.0 };
    std::atomic<double> timeOffset{ 0.0 };

    // tuning callback
    TuneFn     tuneCb;
    UpdateFn   updateCb;

    // Copy the callback out under the lock, then invoke it unlocked.
    void fireUpdate(const TrackSnapshot& s) {
        UpdateFn f;
        { std::lock_guard<std::mutex> lck(cbMtx); f = updateCb; }
        if (f) { f(s); }
    }
    std::mutex cbMtx;

    // snapshot
    TrackSnapshot snap;
    std::mutex    snapMtx;

    std::atomic<bool> running{ false };
    std::thread       worker;
};

} // namespace sattrack
