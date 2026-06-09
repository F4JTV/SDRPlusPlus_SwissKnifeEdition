/*
 * SDR++ Satellite Tracker module
 * --------------------------------
 * Autonomous satellite tracking + Doppler correction for SDR++.
 *
 *  - Orbital propagation, observation and Doppler via libpredict (SGP4/SDP4),
 *    the same engine SatDump uses (src/predict/, GPLv2+).
 *  - TLE catalogue downloaded from CelesTrak and parsed with the SatDump-
 *    derived parser (src/tle_manager.*).
 *  - VFO control follows SDR++'s rigctl_server module: the tracker retunes the
 *    selected VFO directly via tuner::tune (like rigctl_server's "F" handler).
 *  - Pass scheduler (background thread): predicts upcoming passes and tracks each
 *    armed satellite from AOS to LOS in turn, independent of the GUI state.
 *  - Optional JSON-over-TCP output of the sub-satellite point to the ADRASEC
 *    SDR Map (type "satellite"), using the shared TcpLineSender pattern.
 *
 * Author: F4JTV (ADRASEC 06)
 */
#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <gui/tuner.h>
#include <gui/widgets/waterfall.h>
#include <signal_path/signal_path.h>
#include <signal_path/vfo_manager.h>
#include <core.h>
#include <config.h>
#include <utils/flog.h>

#include "tracker_engine.h"
#include "tle_manager.h"
#include "tcp_sender.h"
#include "satellite_freqs.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "satellite_tracker",
    /* Description:     */ "Satellite tracking + Doppler correction (libpredict)",
    /* Author:          */ "F4JTV",
    /* Version:         */ 1, 0, 0,
    /* Max instances    */ 1
};

ConfigManager config;

// CelesTrak groups offered in the source combo.
static const std::vector<std::pair<const char*, const char*>> CELESTRAK_GROUPS = {
    { "Amateur radio",   "amateur"     },
    { "Weather",         "weather"     },
    { "NOAA",            "noaa"        },
    { "Cubesats",        "cubesat"     },
    { "Active",          "active"      },
    { "Space stations",  "stations"    },
    { "GNSS",            "gnss"        },
    { "Custom URL",      ""            },
};

class SatelliteTrackerModule : public ModuleManager::Instance {
public:
    SatelliteTrackerModule(std::string name) {
        this->name = name;

        // ---- Config defaults --------------------------------------------------
        config.acquire();
        if (!config.conf.contains(name)) { config.conf[name] = json::object(); }
        auto& c = config.conf[name];
        if (!c.contains("qthLat"))      { c["qthLat"] = 43.2965; }   // Marseille
        if (!c.contains("qthLon"))      { c["qthLon"] = 5.3698;  }
        if (!c.contains("qthAlt"))      { c["qthAlt"] = 50.0;    }
        if (!c.contains("tleGroupIdx")) { c["tleGroupIdx"] = 0;  }   // amateur
        if (!c.contains("tleCustomURL")){ c["tleCustomURL"] = ""; }
        if (!c.contains("selectedNorad")){ c["selectedNorad"] = -1; }
        if (!c.contains("downlinkHz"))  { c["downlinkHz"] = 145800000.0; }
        if (!c.contains("uplinkHz"))    { c["uplinkHz"] = 0.0; }
        if (!c.contains("controlledVfo")){ c["controlledVfo"] = ""; }
        if (!c.contains("minEl"))       { c["minEl"] = 0.0; }
        if (!c.contains("updateMs"))    { c["updateMs"] = 1000; }
        if (!c.contains("stepHz"))      { c["stepHz"] = 10.0; }
        if (!c.contains("tcpHost"))     { c["tcpHost"] = "127.0.0.1"; }
        if (!c.contains("tcpPort"))     { c["tcpPort"] = 10100; }

        qthLat      = c["qthLat"];
        qthLon      = c["qthLon"];
        qthAlt      = c["qthAlt"];
        tleGroupIdx = c["tleGroupIdx"];
        strncpy(customURL, std::string(c["tleCustomURL"]).c_str(), sizeof(customURL) - 1);
        selectedNorad = c["selectedNorad"];
        downlinkHz  = c["downlinkHz"];
        uplinkHz    = c["uplinkHz"];
        selectedVfo = std::string(c["controlledVfo"]);
        minEl       = c["minEl"];
        updateMs    = c["updateMs"];
        stepHz      = c["stepHz"];
        strncpy(tcpHost, std::string(c["tcpHost"]).c_str(), sizeof(tcpHost) - 1);
        tcpPort     = c["tcpPort"];
        config.release(false);

        // ---- Engine + TLE store ----------------------------------------------
        configRoot = core::args["root"].s();
        // Each source has its own on-disk cache; selecting a source loads that
        // cache (no network). Only "Update TLEs" downloads.
        tleMgr.setStorePath(groupStorePath(tleGroupIdx));
        tleMgr.loadFromDisk();
        rebuildSatList();

        engine.setObserver(qthLat, qthLon, qthAlt);
        engine.setDownlink(downlinkHz);
        engine.setUplink(uplinkHz);
        engine.setMinElevation(minEl);
        engine.setUpdateIntervalMs(updateMs);
        engine.setStepHz(stepHz);
        engine.setTuneCallback([this](double hz) { this->onEngineTune(hz); });
        // Push map/TCP points from the engine's background thread so they keep
        // flowing even when the module panel is collapsed (GUI not drawn).
        engine.setUpdateCallback([this](const sattrack::TrackSnapshot& s) {
            this->maybeSendMapPoint(s);
        });

        // tcp sender target
        tcpSender.setTarget(tcpHost, tcpPort);

        // Apply stored satellite selection (if its TLE is present).
        applyNoradSelection(selectedNorad);
        refreshDlPresets(selectedNorad); // populate preset list, keep saved freq

        gui::menu.registerEntry(name, menuHandler, this, NULL);
    }

    ~SatelliteTrackerModule() {
        gui::menu.removeEntry(name);
        sigpath::vfoManager.onVfoCreated.unbindHandler(&vfoCreatedHandler);
        sigpath::vfoManager.onVfoDeleted.unbindHandler(&vfoDeletedHandler);
        // Stop background threads before tearing down what they touch.
        stopScheduler();          // scheduler drives the engine + module state
        engine.stop();            // its update callback touches tcpSender
        tcpSender.stop();
    }

    void postInit() {
        refreshVfos();
        selectVfoByName(selectedVfo, false);

        vfoCreatedHandler.handler = _vfoCreatedHandler;
        vfoCreatedHandler.ctx = this;
        vfoDeletedHandler.handler = _vfoDeletedHandler;
        vfoDeletedHandler.ctx = this;
        sigpath::vfoManager.onVfoCreated.bindHandler(&vfoCreatedHandler);
        sigpath::vfoManager.onVfoDeleted.bindHandler(&vfoDeletedHandler);

        startScheduler();
    }

    void enable()  { enabled = true; }
    void disable() { enabled = false; }
    bool isEnabled() { return enabled; }

private:
    // ======================= VFO management (from rigctl_server) ==============
    void refreshVfos() {
        std::lock_guard<std::mutex> lck(vfoMtx);
        vfoNames.clear();
        vfoNamesTxt.clear();
        for (auto const& [vfoName, vfo] : gui::waterfall.vfos) {
            vfoNames.push_back(vfoName);
            vfoNamesTxt += vfoName;
            vfoNamesTxt += '\0';
        }
    }

    void selectVfoByName(const std::string& vfoName, bool save = true) {
        std::lock_guard<std::mutex> lck(vfoMtx);
        if (vfoNames.empty()) { selectedVfo = ""; vfoId = 0; return; }
        auto it = std::find(vfoNames.begin(), vfoNames.end(), vfoName);
        if (it != vfoNames.end()) {
            vfoId = (int)std::distance(vfoNames.begin(), it);
            selectedVfo = vfoName;
        }
        else {
            vfoId = 0;
            selectedVfo = vfoNames[0];
        }
        if (save) { saveConf("controlledVfo", selectedVfo); }
    }

    static void _vfoCreatedHandler(VFOManager::VFO* vfo, void* ctx) {
        auto* _this = (SatelliteTrackerModule*)ctx;
        _this->refreshVfos();
        _this->selectVfoByName(_this->selectedVfo, false);
    }
    static void _vfoDeletedHandler(std::string n, void* ctx) {
        auto* _this = (SatelliteTrackerModule*)ctx;
        _this->refreshVfos();
        _this->selectVfoByName(_this->selectedVfo, false);
    }

    double getCurrentVfoFreq() {
        double freq = gui::waterfall.getCenterFrequency();
        std::string vfo;
        { std::lock_guard<std::mutex> lck(vfoMtx); vfo = selectedVfo; }
        if (!vfo.empty() && sigpath::vfoManager.vfoExists(vfo)) {
            freq += sigpath::vfoManager.getOffset(vfo);
        }
        return freq;
    }

    void tuneVfo(double hz) {
        std::string vfo;
        { std::lock_guard<std::mutex> lck(vfoMtx); vfo = selectedVfo; }
        if (vfo.empty()) { return; }
        tuner::tune(tuner::TUNER_MODE_NORMAL, vfo, hz);
    }

    // Called from the engine thread when Doppler correction is active.
    void onEngineTune(double correctedHz) {
        if (!enabled) { return; }
        tuneVfo(correctedHz);
    }

    // ======================= Scheduler =======================================
    // One armed pass: track this satellite from AOS to LOS.
    struct SchedEntry {
        sattrack::TLE tle;
        double aosUnix = 0;
        double losUnix = 0;
        double maxElDeg = 0;
        bool   done = false;
    };

    void startScheduler() {
        if (schedRunning.exchange(true)) { return; }
        schedThread = std::thread(&SatelliteTrackerModule::schedulerLoop, this);
    }
    void stopScheduler() {
        if (!schedRunning.exchange(false)) { return; }
        if (schedThread.joinable()) { schedThread.join(); }
    }

    // Runs on its own thread (independent of the GUI) so scheduled passes are
    // honored even when the module panel is collapsed.
    void schedulerLoop() {
        while (schedRunning.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            if (!schedEnabled.load()) { activeSchedNorad.store(-1); continue; }

            double now = (double)std::time(nullptr);
            SchedEntry active;
            bool haveActive = false;
            {
                std::lock_guard<std::mutex> lck(schedMtx);
                double best = 1e18;
                for (auto& e : schedule) {
                    if (now > e.losUnix) { e.done = true; continue; }
                    // Active window: from AOS (minus lead) to LOS.
                    if (now >= e.aosUnix - schedLeadSec && now <= e.losUnix) {
                        if (e.aosUnix < best) { best = e.aosUnix; active = e; haveActive = true; }
                    }
                }
            }

            int want = haveActive ? active.tle.norad : -1;
            if (want != activeSchedNorad.load()) {
                activeSchedNorad.store(want);
                if (haveActive) {
                    // Point the tracker at the scheduled satellite and let the
                    // engine's Doppler loop take over for the whole pass.
                    engine.setTLE(active.tle);
                    double hz; std::string lbl;
                    if (sattrack::lookupDownlink(active.tle.norad, hz, lbl)) {
                        engine.setDownlink(hz);
                        downlinkHz = hz;
                    }
                    engine.setCorrectionEnabled(true);
                    selectedNorad = active.tle.norad;
                    guiNeedsSatSync.store(true);
                }
                else {
                    // No scheduled pass in progress: hand control back to the
                    // manual Doppler toggle.
                    engine.setCorrectionEnabled(dopplerEnabled);
                }
            }
        }
    }

    // Add the next `count` passes' worth of predictions for a TLE to predList.
    void computePredictions(const sattrack::TLE& tle) {
        predList.clear();
        predSatNorad = tle.norad;
        predSatName = tle.name;
        engine.computePasses(tle, 6, predList);
    }

    // Arm a specific predicted pass for a satellite.
    void schedulePass(const sattrack::TLE& tle, const sattrack::TrackerEngine::PassInfo& p) {
        SchedEntry e;
        e.tle = tle;
        e.aosUnix = p.aosUnix;
        e.losUnix = p.losUnix;
        e.maxElDeg = p.maxElDeg;
        std::lock_guard<std::mutex> lck(schedMtx);
        // Avoid duplicates (same sat + same AOS within 30 s).
        for (auto& s : schedule) {
            if (s.tle.norad == e.tle.norad && std::abs(s.aosUnix - e.aosUnix) < 30) { return; }
        }
        schedule.push_back(e);
        std::sort(schedule.begin(), schedule.end(),
                  [](const SchedEntry& a, const SchedEntry& b) { return a.aosUnix < b.aosUnix; });
    }

    // ======================= TLE / satellite list ============================
    void rebuildSatList() {
        auto reg = tleMgr.all();
        std::lock_guard<std::mutex> lck(satListMtx);
        satRegistry = std::move(reg);
    }

    // Short filesystem-safe key for a source (CelesTrak group id, or "custom").
    std::string groupKey(int idx) {
        if (idx >= 0 && idx < (int)CELESTRAK_GROUPS.size()) {
            std::string k = CELESTRAK_GROUPS[idx].second;
            return k.empty() ? std::string("custom") : k;
        }
        return "default";
    }

    // Per-source on-disk cache path.
    std::string groupStorePath(int idx) {
        return configRoot + "/satellite_tracker_tles_" + groupKey(idx) + ".txt";
    }

    // Point the store at a source's cache and load it into the list (no network).
    void loadGroupFromDisk(int idx) {
        tleMgr.setStorePath(groupStorePath(idx));
        int n = tleMgr.loadFromDisk();
        rebuildSatList();
        applyNoradSelection(selectedNorad);
        refreshDlPresets(selectedNorad);
        tleStatus = (n > 0) ? (std::to_string(n) + " sats (cached)")
                            : std::string("No cached TLEs - click Update");
    }

    void applyNoradSelection(int norad) {
        if (norad < 0) { engine.clearTLE(); return; }
        auto t = tleMgr.byNorad(norad);
        if (t.has_value()) {
            engine.setTLE(t.value());
            selectedNorad = norad;
        }
        else {
            engine.clearTLE();
        }
    }

    // Populate the list of known downlinks for a NORAD (no frequency change).
    void refreshDlPresets(int norad) {
        dlPresets.clear();
        if (norad >= 0) { sattrack::collectDownlinks(norad, dlPresets); }
        dlPresetIdx = 0;
    }

    // Called on *explicit* user selection: fill the downlink from the SatDump/
    // AMSAT table if the satellite is known. Leaves the value untouched if not.
    void autoFillDownlink(int norad) {
        refreshDlPresets(norad);
        if (dlPresets.empty()) {
            freqAutoNote.clear();
            return;
        }
        downlinkHz = dlPresets[0].downlinkHz;
        engine.setDownlink(downlinkHz);
        saveConf("downlinkHz", downlinkHz);
        freqAutoNote = std::string("Auto: ") + dlPresets[0].label;
    }

    void startTleUpdate() {
        if (tleUpdating.exchange(true)) { return; }
        // Download into the current source's own cache file.
        tleMgr.setStorePath(groupStorePath(tleGroupIdx));
        std::string url;
        if (tleGroupIdx >= 0 && tleGroupIdx < (int)CELESTRAK_GROUPS.size() &&
            std::strlen(CELESTRAK_GROUPS[tleGroupIdx].second) > 0) {
            url = sattrack::TLEManager::celestrakGroupURL(CELESTRAK_GROUPS[tleGroupIdx].second);
        }
        else {
            url = std::string(customURL);
        }
        tleStatus = "Downloading...";
        std::thread([this, url]() {
            int n = tleMgr.downloadReplace(url);
            if (n > 0) {
                rebuildSatList();
                applyNoradSelection(selectedNorad);
                tleStatus = "Loaded " + std::to_string((int)tleMgr.size()) + " sats";
            }
            else if (n == 0) {
                tleStatus = tleMgr.lastError();
                if (tleStatus.empty()) { tleStatus = "No valid TLEs at URL"; }
            }
            else {
                tleStatus = tleMgr.lastError();
                if (tleStatus.empty()) { tleStatus = "Download failed"; }
            }
            tleUpdating = false;
        }).detach();
    }

    void startNoradAdd(int norad) {
        if (norad <= 0) { return; }
        if (tleUpdating.exchange(true)) { return; }
        std::string url = sattrack::TLEManager::celestrakCatnrURL(norad);
        tleStatus = "Fetching " + std::to_string(norad) + "...";
        std::thread([this, url, norad]() {
            int n = tleMgr.downloadMerge(url);
            if (n > 0) {
                rebuildSatList();
                applyNoradSelection(norad);
                tleStatus = "Added NORAD " + std::to_string(norad);
            }
            else {
                std::string e = tleMgr.lastError();
                tleStatus = e.empty() ? ("Could not fetch " + std::to_string(norad))
                                      : ("NORAD " + std::to_string(norad) + ": " + e);
            }
            tleUpdating = false;
        }).detach();
    }

    // ======================= Map output ======================================
    void maybeSendMapPoint(const sattrack::TrackSnapshot& s) {
        if (!tcpSender.isRunning()) { return; }
        if (!s.valid || s.norad < 0) { return; }

        int64_t now = (int64_t)std::time(nullptr);
        if (now - lastMapSend < 2) { return; } // throttle to ~0.5 Hz
        lastMapSend = now;

        std::time_t tt = (std::time_t)now;
        std::tm tmv;
#if defined(_WIN32)
        gmtime_s(&tmv, &tt);
#else
        gmtime_r(&tt, &tmv);
#endif
        char dbuf[16], tbuf[16];
        std::strftime(dbuf, sizeof(dbuf), "%Y-%m-%d", &tmv);
        std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmv);

        char line[640];
        snprintf(line, sizeof(line),
                 "{\"name\":\"%s\",\"date\":\"%s\",\"time\":\"%s\","
                 "\"lat\":%.6f,\"lon\":%.6f,\"type\":\"satellite\","
                 "\"speed\":null,\"info\":\"norad=%d;alt=%.0f;az=%.1f;el=%.1f;"
                 "range=%.0f;doppler=%.0f;footprint=%.0f\"}",
                 s.satName.c_str(), dbuf, tbuf, s.subLat, s.subLon,
                 s.norad, s.altKm, s.az, s.el, s.range, s.dopplerHz, s.footprintKm);
        tcpSender.send(std::string(line));
    }

    // ======================= Helpers =========================================
    void saveConf(const char* key, const json& val) {
        config.acquire();
        config.conf[name][key] = val;
        config.release(true);
    }

    static std::string fmtCountdown(int64_t target, int64_t now) {
        if (target <= 0) { return "--:--:--"; }
        int64_t d = target - now;
        bool past = d < 0;
        if (past) { d = -d; }
        int h = (int)(d / 3600); d %= 3600;
        int m = (int)(d / 60);
        int sec = (int)(d % 60);
        char buf[32];
        snprintf(buf, sizeof(buf), "%s%02d:%02d:%02d", past ? "-" : "", h, m, sec);
        return buf;
    }

    static std::string fmtClock(int64_t unix) {
        if (unix <= 0) { return "--"; }
        std::time_t tt = (std::time_t)unix;
        std::tm tmv;
#if defined(_WIN32)
        localtime_s(&tmv, &tt);
#else
        localtime_r(&tt, &tmv);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%H:%M:%S", &tmv);
        return buf;
    }

    // ======================= GUI =============================================
    static void menuHandler(void* ctx) {
        auto* _this = (SatelliteTrackerModule*)ctx;
        _this->draw();
    }

    // Format a unix time as compact UTC "MM-DD HH:MM" for the schedule UI.
    static void fmtClock(double unix, char* buf, size_t n) {
        std::time_t tt = (std::time_t)unix;
        std::tm tmv;
#if defined(_WIN32)
        gmtime_s(&tmv, &tt);
#else
        gmtime_r(&tt, &tmv);
#endif
        std::strftime(buf, n, "%m-%d %H:%MZ", &tmv);
    }

    void draw() {
        float w = ImGui::GetContentRegionAvail().x;

        // If the scheduler switched the tracked satellite on its own thread,
        // refresh the GUI-side selection state (presets, search box) here.
        if (guiNeedsSatSync.exchange(false)) {
            refreshDlPresets(selectedNorad);
        }

        auto snap = engine.snapshot();
        int64_t now = (int64_t)std::time(nullptr);

        // Note: map/TCP points are pushed from the engine thread (setUpdateCallback),
        // not from here, so they keep flowing when this panel is collapsed.

        // ---------------- Observer (QTH) -------------------------------------
        if (ImGui::CollapsingHeader("Observer (QTH)", ImGuiTreeNodeFlags_DefaultOpen)) {
            bool changed = false;
            ImGui::LeftLabel("Latitude");
            ImGui::FillWidth();
            if (ImGui::InputDouble(CONCAT("##sat_lat_", name), &qthLat, 0, 0, "%.4f")) { changed = true; }
            ImGui::LeftLabel("Longitude");
            ImGui::FillWidth();
            if (ImGui::InputDouble(CONCAT("##sat_lon_", name), &qthLon, 0, 0, "%.4f")) { changed = true; }
            ImGui::LeftLabel("Altitude (m)");
            ImGui::FillWidth();
            if (ImGui::InputDouble(CONCAT("##sat_alt_", name), &qthAlt, 0, 0, "%.0f")) { changed = true; }
            if (changed) {
                engine.setObserver(qthLat, qthLon, qthAlt);
                saveConf("qthLat", qthLat);
                saveConf("qthLon", qthLon);
                saveConf("qthAlt", qthAlt);
            }
        }

        // ---------------- TLE catalogue --------------------------------------
        if (ImGui::CollapsingHeader("TLE catalogue", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::LeftLabel("Source");
            ImGui::FillWidth();
            std::string combo;
            for (auto& g : CELESTRAK_GROUPS) { combo += g.first; combo += '\0'; }
            if (ImGui::Combo(CONCAT("##sat_tle_src_", name), &tleGroupIdx, combo.c_str())) {
                saveConf("tleGroupIdx", tleGroupIdx);
                // Show that source's cached satellites immediately (no download).
                // Use "Update TLEs" to refresh from CelesTrak.
                loadGroupFromDisk(tleGroupIdx);
            }
            bool isCustom = (tleGroupIdx == (int)CELESTRAK_GROUPS.size() - 1);
            if (isCustom) {
                ImGui::LeftLabel("URL");
                ImGui::FillWidth();
                if (ImGui::InputText(CONCAT("##sat_tle_url_", name), customURL, sizeof(customURL))) {
                    saveConf("tleCustomURL", std::string(customURL));
                }
            }

            bool busy = tleUpdating.load();
            if (busy) { style::beginDisabled(); }
            if (ImGui::Button(CONCAT("Update TLEs##", name), ImVec2(w * 0.5f - 4, 0))) {
                startTleUpdate();
            }
            ImGui::SameLine();
            if (ImGui::Button(CONCAT("Reload disk##", name), ImVec2(w * 0.5f - 4, 0))) {
                loadGroupFromDisk(tleGroupIdx);
            }

            ImGui::LeftLabel("Add NORAD");
            ImGui::SetNextItemWidth(w * 0.45f);
            ImGui::InputInt(CONCAT("##sat_norad_add_", name), &noradToAdd, 0, 0);
            ImGui::SameLine();
            if (ImGui::Button(CONCAT("Fetch##norad", name), ImVec2(w * 0.3f, 0))) {
                startNoradAdd(noradToAdd);
            }
            if (busy) { style::endDisabled(); }

            ImGui::TextUnformatted(("Catalogue: " + std::to_string((int)tleMgr.size()) + " objects").c_str());
            if (!tleStatus.empty()) {
                ImGui::TextColored(ImVec4(0.6f, 0.7f, 0.9f, 1.0f), "%s", tleStatus.c_str());
            }
        }

        // ---------------- Satellite selection --------------------------------
        if (ImGui::CollapsingHeader("Satellite", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::LeftLabel("Search");
            ImGui::FillWidth();
            ImGui::InputText(CONCAT("##sat_search_", name), searchBuf, sizeof(searchBuf));

            std::vector<sattrack::TLE> list;
            { std::lock_guard<std::mutex> lck(satListMtx); list = satRegistry; }

            // Filter by search.
            std::string needle = searchBuf;
            std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);
            std::vector<int> filtIdx;
            for (int i = 0; i < (int)list.size(); i++) {
                if (needle.empty()) { filtIdx.push_back(i); continue; }
                std::string nm = list[i].name;
                std::transform(nm.begin(), nm.end(), nm.begin(), ::tolower);
                if (nm.find(needle) != std::string::npos ||
                    std::to_string(list[i].norad).find(needle) != std::string::npos) {
                    filtIdx.push_back(i);
                }
            }

            ImGui::LeftLabel("Track");
            ImGui::FillWidth();
            std::string label = (selectedNorad >= 0 && snap.haveTLE) ? snap.satName : "(none)";
            if (ImGui::BeginCombo(CONCAT("##sat_sel_", name), label.c_str())) {
                for (int idx : filtIdx) {
                    bool sel = (list[idx].norad == selectedNorad);
                    std::string item = list[idx].name + " [" + std::to_string(list[idx].norad) + "]";
                    if (ImGui::Selectable(CONCAT(item.c_str(), std::to_string(idx)), sel)) {
                        selectedNorad = list[idx].norad;
                        saveConf("selectedNorad", selectedNorad);
                        applyNoradSelection(selectedNorad);
                        autoFillDownlink(selectedNorad);
                    }
                    if (sel) { ImGui::SetItemDefaultFocus(); }
                }
                ImGui::EndCombo();
            }
        }

        // ---------------- Frequencies ----------------------------------------
        if (ImGui::CollapsingHeader("Frequencies", ImGuiTreeNodeFlags_DefaultOpen)) {
            double dlMHz = downlinkHz / 1e6;
            ImGui::LeftLabel("Downlink (MHz)");
            ImGui::FillWidth();
            if (ImGui::InputDouble(CONCAT("##sat_dl_", name), &dlMHz, 0, 0, "%.6f")) {
                downlinkHz = dlMHz * 1e6;
                engine.setDownlink(downlinkHz);
                saveConf("downlinkHz", downlinkHz);
                freqAutoNote.clear(); // user override
            }
            // Known downlinks for the selected sat (SatDump / AMSAT). Lets the
            // user switch between e.g. APT and HRPT without retyping.
            if (dlPresets.size() > 1) {
                std::string pc;
                for (auto& p : dlPresets) {
                    char b[96];
                    snprintf(b, sizeof(b), "%s  (%.4f MHz)", p.label, p.downlinkHz / 1e6);
                    pc += b; pc += '\0';
                }
                ImGui::LeftLabel("Preset");
                ImGui::FillWidth();
                if (ImGui::Combo(CONCAT("##sat_dl_preset_", name), &dlPresetIdx, pc.c_str())) {
                    if (dlPresetIdx >= 0 && dlPresetIdx < (int)dlPresets.size()) {
                        downlinkHz = dlPresets[dlPresetIdx].downlinkHz;
                        engine.setDownlink(downlinkHz);
                        saveConf("downlinkHz", downlinkHz);
                        freqAutoNote = std::string("Auto: ") + dlPresets[dlPresetIdx].label;
                    }
                }
            }
            if (!freqAutoNote.empty()) {
                ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "%s", freqAutoNote.c_str());
            }
            double ulMHz = uplinkHz / 1e6;
            ImGui::LeftLabel("Uplink (MHz)");
            ImGui::FillWidth();
            if (ImGui::InputDouble(CONCAT("##sat_ul_", name), &ulMHz, 0, 0, "%.6f")) {
                uplinkHz = ulMHz * 1e6;
                engine.setUplink(uplinkHz);
                saveConf("uplinkHz", uplinkHz);
            }
            if (ImGui::Button(CONCAT("Set downlink = current VFO##", name), ImVec2(w, 0))) {
                downlinkHz = getCurrentVfoFreq();
                engine.setDownlink(downlinkHz);
                saveConf("downlinkHz", downlinkHz);
            }
        }

        // ---------------- VFO control + Doppler ------------------------------
        if (ImGui::CollapsingHeader("Doppler correction", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::LeftLabel("Controlled VFO");
            ImGui::FillWidth();
            {
                std::lock_guard<std::mutex> lck(vfoMtx);
                if (ImGui::Combo(CONCAT("##sat_vfo_", name), &vfoId, vfoNamesTxt.c_str())) {
                    if (vfoId >= 0 && vfoId < (int)vfoNames.size()) {
                        selectedVfo = vfoNames[vfoId];
                        saveConf("controlledVfo", selectedVfo);
                    }
                }
            }

            if (ImGui::Checkbox(CONCAT("Enable Doppler tracking##", name), &dopplerEnabled)) {
                engine.setCorrectionEnabled(dopplerEnabled);
            }
            ImGui::SameLine();
            if (!dopplerEnabled) {
                ImGui::TextDisabled("off");
            }
            else if (snap.aboveHorizon) {
                ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "tracking");
            }
            else {
                ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "armed (below horizon)");
            }

            ImGui::LeftLabel("Min elevation");
            ImGui::FillWidth();
            float minElF = (float)minEl;
            if (ImGui::SliderFloat(CONCAT("##sat_minel_", name), &minElF, 0.0f, 30.0f, "%.0f deg")) {
                minEl = (double)minElF;
                engine.setMinElevation(minEl);
                saveConf("minEl", minEl);
            }
            ImGui::LeftLabel("Update (ms)");
            ImGui::FillWidth();
            if (ImGui::InputInt(CONCAT("##sat_upms_", name), &updateMs, 0, 0)) {
                if (updateMs < 100) { updateMs = 100; }
                engine.setUpdateIntervalMs(updateMs);
                saveConf("updateMs", updateMs);
            }
            ImGui::LeftLabel("Step (Hz)");
            ImGui::FillWidth();
            if (ImGui::InputDouble(CONCAT("##sat_step_", name), &stepHz, 0, 0, "%.0f")) {
                if (stepHz < 0) { stepHz = 0; }
                engine.setStepHz(stepHz);
                saveConf("stepHz", stepHz);
            }
        }

        // ---------------- Live tracking --------------------------------------
        if (ImGui::CollapsingHeader("Tracking", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (!snap.haveTLE) {
                ImGui::TextDisabled("Select a satellite to begin tracking.");
            }
            else if (!snap.valid) {
                ImGui::TextDisabled("Computing...");
            }
            else {
                if (ImGui::BeginTable(CONCAT("##sat_track_tbl_", name), 2,
                                      ImGuiTableFlags_SizingStretchProp)) {
                    auto row = [](const char* k, const std::string& v) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(k);
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(v.c_str());
                    };
                    char b[64];

                    snprintf(b, sizeof(b), "%.1f deg", snap.az);   row("Azimuth", b);
                    // Elevation, coloured by visibility.
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted("Elevation");
                    ImGui::TableNextColumn();
                    snprintf(b, sizeof(b), "%.1f deg", snap.el);
                    if (snap.el > 0) { ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "%s", b); }
                    else             { ImGui::TextDisabled("%s", b); }

                    snprintf(b, sizeof(b), "%.0f km", snap.range);          row("Range", b);
                    snprintf(b, sizeof(b), "%.3f km/s", snap.rangeRate);    row("Range rate", b);
                    snprintf(b, sizeof(b), "%+.0f Hz", snap.dopplerHz);     row("Doppler (DL)", b);
                    snprintf(b, sizeof(b), "%.6f MHz", snap.correctedDownlink / 1e6); row("Corrected DL", b);
                    if (uplinkHz > 0) {
                        snprintf(b, sizeof(b), "%.6f MHz", snap.correctedUplink / 1e6);
                        row("Corrected UL", b);
                    }
                    snprintf(b, sizeof(b), "%.3f, %.3f", snap.subLat, snap.subLon); row("Sub-point", b);
                    snprintf(b, sizeof(b), "%.0f km", snap.altKm);          row("Altitude", b);
                    snprintf(b, sizeof(b), "%.0f km", snap.footprintKm);    row("Footprint", b);
                    row("Eclipse", snap.eclipsed ? "in shadow" : "sunlit");

                    if (snap.decayed) { row("Status", "DECAYED"); }

                    // Pass timing
                    if (snap.aboveHorizon) {
                        row("LOS in", fmtCountdown(snap.nextLOS, now) + " (" + fmtClock(snap.nextLOS) + ")");
                    }
                    else {
                        row("AOS in", fmtCountdown(snap.nextAOS, now) + " (" + fmtClock(snap.nextAOS) + ")");
                    }
                    snprintf(b, sizeof(b), "%.1f deg", snap.maxEl);         row("Max elevation", b);

                    ImGui::EndTable();
                }
            }
        }

        // ---------------- Scheduler ------------------------------------------
        if (ImGui::CollapsingHeader("Scheduler")) {
            // Enable toggle + status.
            bool en = schedEnabled.load();
            if (ImGui::Checkbox(CONCAT("Enable scheduler##", name), &en)) {
                schedEnabled.store(en);
                if (!en) { engine.setCorrectionEnabled(dopplerEnabled); }
            }
            ImGui::SameLine();
            int actN = activeSchedNorad.load();
            if (!en) {
                ImGui::TextDisabled("off");
            }
            else if (actN >= 0) {
                ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "tracking %d", actN);
            }
            else {
                size_t n; { std::lock_guard<std::mutex> lck(schedMtx); n = schedule.size(); }
                ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "armed (%d)", (int)n);
            }
            ImGui::TextDisabled("Tracks each armed pass from AOS to LOS, in order.");

            // Predict passes for the satellite currently selected in 'Satellite'.
            auto selTle = tleMgr.byNorad(selectedNorad);
            if (!selTle.has_value()) {
                ImGui::TextDisabled("Select a satellite above to predict its passes.");
            }
            else {
                if (ImGui::Button(CONCAT("Compute passes##sched_", name), ImVec2(w, 0))) {
                    computePredictions(selTle.value());
                }
                if (!predList.empty() && predSatNorad == selectedNorad) {
                    ImGui::TextDisabled("Next passes for %s:", predSatName.c_str());
                    for (size_t i = 0; i < predList.size(); i++) {
                        const auto& p = predList[i];
                        char ab[24]; fmtClock(p.aosUnix, ab, sizeof(ab));
                        int durMin = (int)((p.losUnix - p.aosUnix) / 60.0);
                        ImGui::Text("%s  el %2.0f\xC2\xB0  %dm", ab, p.maxElDeg, durMin);
                        ImGui::SameLine();
                        if (ImGui::Button(CONCAT(CONCAT("Arm##sched_arm_", name), std::to_string(i)),
                                          ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                            schedulePass(selTle.value(), p);
                        }
                    }
                }
            }

            // Armed schedule.
            ImGui::Spacing();
            std::vector<SchedEntry> snapSched;
            { std::lock_guard<std::mutex> lck(schedMtx); snapSched = schedule; }
            if (snapSched.empty()) {
                ImGui::TextDisabled("No passes armed.");
            }
            else if (ImGui::BeginTable(CONCAT("##sched_tbl_", name), 5,
                         ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Satellite");
                ImGui::TableSetupColumn("AOS");
                ImGui::TableSetupColumn("El");
                ImGui::TableSetupColumn("Status");
                ImGui::TableSetupColumn("");
                ImGui::TableHeadersRow();

                double now = (double)std::time(nullptr);
                int removeIdx = -1;
                for (size_t i = 0; i < snapSched.size(); i++) {
                    const auto& e = snapSched[i];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(e.tle.name.c_str());
                    char ab[24]; fmtClock(e.aosUnix, ab, sizeof(ab));
                    ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(ab);
                    ImGui::TableSetColumnIndex(2); ImGui::Text("%.0f\xC2\xB0", e.maxElDeg);
                    ImGui::TableSetColumnIndex(3);
                    if (e.tle.norad == activeSchedNorad.load()) {
                        ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "ACTIVE");
                    }
                    else if (now > e.losUnix) {
                        ImGui::TextDisabled("done");
                    }
                    else {
                        int mins = (int)((e.aosUnix - now) / 60.0);
                        if (mins >= 0) { ImGui::Text("in %dm", mins); }
                        else { ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "passing"); }
                    }
                    ImGui::TableSetColumnIndex(4);
                    if (ImGui::SmallButton(CONCAT(CONCAT("X##sched_del_", name), std::to_string(i)))) {
                        removeIdx = (int)i;
                    }
                }
                ImGui::EndTable();

                if (removeIdx >= 0) {
                    std::lock_guard<std::mutex> lck(schedMtx);
                    if (removeIdx < (int)schedule.size()) { schedule.erase(schedule.begin() + removeIdx); }
                }
            }
            if (!snapSched.empty()) {
                if (ImGui::Button(CONCAT("Clear finished##sched_", name), ImVec2(w, 0))) {
                    double now = (double)std::time(nullptr);
                    std::lock_guard<std::mutex> lck(schedMtx);
                    schedule.erase(std::remove_if(schedule.begin(), schedule.end(),
                                   [&](const SchedEntry& e) { return now > e.losUnix; }),
                                   schedule.end());
                }
            }
        }

        // ---------------- Map output (TCP/JSON) ------------------------------
        if (ImGui::CollapsingHeader("Map output (TCP)")) {
            bool on = tcpSender.isRunning();
            if (on) { style::beginDisabled(); }
            ImGui::LeftLabel("Host");
            ImGui::FillWidth();
            if (ImGui::InputText(CONCAT("##sat_tcp_host_", name), tcpHost, sizeof(tcpHost))) {
                saveConf("tcpHost", std::string(tcpHost));
                tcpSender.setTarget(tcpHost, tcpPort);
            }
            ImGui::LeftLabel("Port");
            ImGui::FillWidth();
            if (ImGui::InputInt(CONCAT("##sat_tcp_port_", name), &tcpPort, 0, 0)) {
                saveConf("tcpPort", tcpPort);
                tcpSender.setTarget(tcpHost, tcpPort);
            }
            if (on) { style::endDisabled(); }

            bool en = on;
            if (ImGui::Checkbox(CONCAT("Enable TCP##", name), &en)) {
                if (en) { tcpSender.setTarget(tcpHost, tcpPort); tcpSender.start(); }
                else    { tcpSender.stop(); }
            }
            ImGui::SameLine();
            if (!on)                       { ImGui::TextDisabled("disabled"); }
            else if (tcpSender.isConnected()) { ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "connected"); }
            else                           { ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "enabled"); }
        }
    }

    // ======================= Members =========================================
    std::string name;
    bool enabled = true;

    // engine + data
    sattrack::TrackerEngine engine;
    sattrack::TLEManager    tleMgr;
    std::string             configRoot;
    std::vector<sattrack::TLE> satRegistry;
    std::mutex              satListMtx;

    TcpLineSender           tcpSender;

    // QTH
    double qthLat = 43.2965, qthLon = 5.3698, qthAlt = 50.0;

    // TLE source
    int  tleGroupIdx = 0;
    char customURL[512] = "";
    std::atomic<bool> tleUpdating{ false };
    std::string tleStatus;
    int  noradToAdd = 0;

    // satellite + freqs
    int    selectedNorad = -1;
    char   searchBuf[128] = "";
    double downlinkHz = 145800000.0;
    double uplinkHz = 0.0;
    std::vector<sattrack::SatFreq> dlPresets; // known downlinks for selected sat
    int    dlPresetIdx = 0;
    std::string freqAutoNote;                 // "Auto: NOAA-19 APT", etc.

    // VFO control
    std::vector<std::string> vfoNames;
    std::string vfoNamesTxt;
    std::string selectedVfo;
    int vfoId = 0;
    std::mutex vfoMtx;
    EventHandler<VFOManager::VFO*> vfoCreatedHandler;
    EventHandler<std::string> vfoDeletedHandler;

    // doppler params (dopplerEnabled NOT persisted: never auto-tune at startup)
    bool   dopplerEnabled = false;
    double minEl = 0.0;
    int    updateMs = 1000;
    double stepHz = 10.0;

    // scheduler (enabled state not persisted: never auto-track at startup)
    std::vector<SchedEntry>           schedule;       // armed passes (guarded)
    std::mutex                        schedMtx;
    std::thread                       schedThread;
    std::atomic<bool>                 schedRunning{ false };
    std::atomic<bool>                 schedEnabled{ false };
    std::atomic<int>                  activeSchedNorad{ -1 };
    std::atomic<bool>                 guiNeedsSatSync{ false };
    int                               schedLeadSec = 0; // start at AOS
    // prediction list (GUI thread only)
    std::vector<sattrack::TrackerEngine::PassInfo> predList;
    int                               predSatNorad = -1;
    std::string                       predSatName;

    // tcp map output (enabled state not persisted)
    char tcpHost[256] = "127.0.0.1";
    int  tcpPort = 10100;
    int64_t lastMapSend = 0;
};

MOD_EXPORT void _INIT_() {
    config.setPath(core::args["root"].s() + "/satellite_tracker_config.json");
    config.load(json::object());
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new SatelliteTrackerModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (SatelliteTrackerModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
