#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <gui/main_window.h>
#include <gui/tuner.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <utils/flog.h>
#include "decoder.h"
#include "tcp_sender.h"
#include "adsb/adsb.h"

#include <map>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <ctime>
#include <vector>
#include <algorithm>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "adsb_decoder",
    /* Description:     */ "ADS-B (1090 MHz Mode S) decoder with TCP map output",
    /* Author:          */ "SDR++ Community",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

ConfigManager config;

// Wall-clock seconds (monotonic) for ageing aircraft and pairing CPR frames.
static double nowSec() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

// Per-aircraft tracking state.
struct Aircraft {
    uint32_t icao = 0;
    std::string callsign;
    std::string category;  // Wake/category code, e.g. "A3"; empty if unknown

    // CPR halves cached by frame parity, with their reception times.
    uint32_t cprLatEven = 0, cprLonEven = 0;
    uint32_t cprLatOdd = 0, cprLonOdd = 0;
    bool haveEven = false, haveOdd = false;
    double tEven = 0, tOdd = 0;
    bool surface = false;

    bool havePos = false;
    double lat = 0, lon = 0;

    bool haveAlt = false; int altFt = 0;
    bool haveSpeed = false; double speedKt = 0; bool airspeed = false;
    bool haveHeading = false; double headingDeg = 0;
    bool haveVrate = false; int vrateFpm = 0;

    double lastSeen = 0;
};

class ADSBDecoderModule : public ModuleManager::Instance {
public:
    ADSBDecoderModule(std::string name) {
        this->name = name;

        // Load config.
        config.acquire();
        if (config.conf[name].contains("tcpHost")) tcpHost = config.conf[name]["tcpHost"];
        if (config.conf[name].contains("tcpPort")) tcpPort = config.conf[name]["tcpPort"];
        if (config.conf[name].contains("tcpEnabled")) tcpEnabled = config.conf[name]["tcpEnabled"];
        if (config.conf[name].contains("useRef")) useRef = config.conf[name]["useRef"];
        if (config.conf[name].contains("refLat")) refLat = config.conf[name]["refLat"];
        if (config.conf[name].contains("refLon")) refLon = config.conf[name]["refLon"];
        config.release();
        strncpy(hostBuf, tcpHost.c_str(), sizeof(hostBuf) - 1);

        // Create VFO at the carrier (offset 0, REF_CENTER), 2 MHz wide.
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER,
                                            0, ADSB_BANDWIDTH, ADSB_SAMPLERATE,
                                            ADSB_BANDWIDTH, ADSB_BANDWIDTH, true);
        vfo->setSnapInterval(1);

        decoder = std::make_unique<ADSBDecoder>(name, vfo);
        decoder->onMessage = [this](const adsb::Message& m, const adsb::RawFrame& f) {
            this->onMessage(m, f);
        };
        decoder->start();

        // TCP is intentionally NOT started here, even if `tcpEnabled` was
        // persisted as true. Network connections must only open on explicit
        // user action (clicking the "Enable TCP" checkbox below). We force the
        // checkbox back to its safe default so SDR++ never silently dials out
        // on launch.
        tcpEnabled = false;

        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~ADSBDecoderModule() {
        gui::menu.removeEntry(name);
        if (enabled) {
            decoder->stop();
            decoder.reset();
            sigpath::vfoManager.deleteVFO(vfo);
        }
        tcp.stop();
        sigpath::sinkManager.unregisterStream(name);
    }

    void postInit() {}

    void enable() {
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER,
                                            0, ADSB_BANDWIDTH, ADSB_SAMPLERATE,
                                            ADSB_BANDWIDTH, ADSB_BANDWIDTH, true);
        vfo->setSnapInterval(1);
        decoder->setVFO(vfo);
        decoder->start();
        enabled = true;
    }

    void disable() {
        decoder->stop();
        sigpath::vfoManager.deleteVFO(vfo);
        enabled = false;
    }

    bool isEnabled() { return enabled; }

private:
    // ---- Message handling (runs on the DSP thread) ----
    void onMessage(const adsb::Message& m, const adsb::RawFrame& f) {
        if (m.kind == adsb::KIND_UNKNOWN) { return; } // DF11/other: ignore for now
        double t = nowSec();

        std::lock_guard<std::mutex> lck(acMtx);
        bool isNew = (aircraft.find(m.icao) == aircraft.end());
        Aircraft& ac = aircraft[m.icao];
        ac.icao = m.icao;
        ac.lastSeen = t;
        if (isNew) { order.push_back(m.icao); } // keep first-seen order for display

        switch (m.kind) {
        case adsb::KIND_IDENT:
            if (!m.callsign.empty()) { ac.callsign = m.callsign; }
            // Aircraft category code: the typeCode selects the set letter
            // (TC=4 -> 'A', 3 -> 'B', 2 -> 'C', 1 -> 'D'), and the 3-bit
            // category subcode (1-7) gives the sub-type. Subcode 0 means
            // "no category information available" -> leave empty.
            if (m.typeCode >= 1 && m.typeCode <= 4 && m.category >= 1 && m.category <= 7) {
                char buf[4];
                buf[0] = "DCBA"[m.typeCode - 1];
                buf[1] = '0' + m.category;
                buf[2] = 0;
                ac.category = buf;
            }
            break;

        case adsb::KIND_VELOCITY:
            if (m.hasGroundSpeed) { ac.haveSpeed = true; ac.speedKt = m.speedKt; ac.airspeed = m.airspeed; }
            if (m.hasHeading)     { ac.haveHeading = true; ac.headingDeg = m.headingDeg; }
            if (m.hasVerticalRate){ ac.haveVrate = true; ac.vrateFpm = m.verticalRateFpm; }
            break;

        case adsb::KIND_AIRBORNE_POS:
        case adsb::KIND_SURFACE_POS: {
            ac.surface = (m.kind == adsb::KIND_SURFACE_POS);
            if (m.hasAltitude) { ac.haveAlt = true; ac.altFt = m.altitudeFt; }
            if (m.hasGroundSpeed) { ac.haveSpeed = true; ac.speedKt = m.speedKt; ac.airspeed = m.airspeed; }

            if (m.cprOddFrame) { ac.cprLatOdd = m.cprLat; ac.cprLonOdd = m.cprLon; ac.haveOdd = true; ac.tOdd = t; }
            else               { ac.cprLatEven = m.cprLat; ac.cprLonEven = m.cprLon; ac.haveEven = true; ac.tEven = t; }

            bool fix = false;
            double lat = 0, lon = 0;
            // Prefer a global decode from a fresh even/odd pair (<10 s apart).
            if (ac.haveEven && ac.haveOdd && std::fabs(ac.tEven - ac.tOdd) < 10.0) {
                bool mostRecentOdd = ac.tOdd >= ac.tEven;
                fix = adsb::cprDecodeGlobal(ac.cprLatEven, ac.cprLonEven,
                                            ac.cprLatOdd, ac.cprLonOdd,
                                            mostRecentOdd, ac.surface, lat, lon);
            }
            // Otherwise, a local decode if a reference position is configured.
            if (!fix && useRef) {
                fix = adsb::cprDecodeLocal(m.cprLat, m.cprLon, m.cprOddFrame, ac.surface,
                                           refLat, refLon, lat, lon);
            }
            if (fix && std::fabs(lat) <= 90.0 && std::fabs(lon) <= 180.0) {
                ac.havePos = true; ac.lat = lat; ac.lon = lon;
                emitContact(ac);
            }
            break;
        }
        default: break;
        }
    }

    // Build a JSON line and forward it over TCP. Schema mirrors the AIS module:
    //   name, icao, date, time, lat, lon, type=ADSB, speed (nullable), info.
    void emitContact(const Aircraft& ac) {
        if (!tcpEnabled || !ac.havePos) { return; }

        // UTC date/time.
        std::time_t tt = std::time(nullptr);
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &tt);
#else
        gmtime_r(&tt, &utc);
#endif
        char dbuf[16], tbuf[16];
        std::strftime(dbuf, sizeof(dbuf), "%Y-%m-%d", &utc);
        std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &utc);

        char icaoStr[8];
        std::snprintf(icaoStr, sizeof(icaoStr), "%06X", ac.icao);

        std::string nameStr = ac.callsign.empty()
            ? (std::string("ICAO:") + icaoStr) : ac.callsign;

        // Speed: knots if known, else null.
        char speedStr[32];
        if (ac.haveSpeed) { std::snprintf(speedStr, sizeof(speedStr), "%.1f", ac.speedKt); }
        else              { std::snprintf(speedStr, sizeof(speedStr), "null"); }

        // Simplified free-form info field: only hdg + alt_ft.
        // (The map distinguishes aircraft via icao/category, so the legacy
        // verbose info was no longer useful; callsign is already in "name",
        // vertical rate and airspeed flag are dropped.)
        std::string info;
        if (ac.haveHeading) {
            char h[32]; std::snprintf(h, sizeof(h), "hdg=%.0f", ac.headingDeg);
            info += h;
        }
        if (ac.haveAlt) {
            if (!info.empty()) info += " ";
            info += "alt_ft=" + std::to_string(ac.altFt);
        }

        // Category: "A3" / "B1" / ... or null when unknown.
        char categoryStr[16];
        if (ac.category.empty()) {
            std::snprintf(categoryStr, sizeof(categoryStr), "null");
        } else {
            std::snprintf(categoryStr, sizeof(categoryStr), "\"%s\"", ac.category.c_str());
        }

        char buf[640];
        std::snprintf(buf, sizeof(buf),
            "{\"name\":\"%s\",\"icao\":\"%s\",\"date\":\"%s\",\"time\":\"%s\","
            "\"lat\":%.6f,\"lon\":%.6f,\"type\":\"ADSB\",\"speed\":%s,"
            "\"category\":%s,\"info\":\"%s\"}",
            nameStr.c_str(), icaoStr, dbuf, tbuf, ac.lat, ac.lon, speedStr,
            categoryStr, info.c_str());

        tcp.send(buf);
    }

    void pruneAircraft(double maxAgeSec) {
        double t = nowSec();
        for (auto it = aircraft.begin(); it != aircraft.end(); ) {
            if (t - it->second.lastSeen > maxAgeSec) {
                uint32_t dead = it->first;
                it = aircraft.erase(it);
                order.erase(std::remove(order.begin(), order.end(), dead), order.end());
            }
            else { ++it; }
        }
    }

    // ---- GUI ----
    static void menuHandler(void* ctx) {
        ADSBDecoderModule* _this = (ADSBDecoderModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) { style::beginDisabled(); }

        // One-click tuning: retune the SDR so this module's VFO sits exactly on
        // 1090 MHz. Since the VFO is created with offset 0 and REF_CENTER,
        // tuning the VFO to 1090 MHz puts the SDR center on 1090 MHz too.
        if (ImGui::Button(("Tune to 1090 MHz##adsb_tune_" + _this->name).c_str(), ImVec2(menuWidth, 0))) {
            tuner::tune(tuner::TUNER_MODE_NORMAL, _this->name, 1090000000.0);
        }
        ImGui::TextDisabled("SDR sample rate must be >= 2 MHz.");

        ImGui::Separator();

        // --- TCP output ---
        ImGui::Text("TCP map output");
        ImGui::LeftLabel("Host");
        ImGui::FillWidth();
        if (ImGui::InputText(("##adsb_host_" + _this->name).c_str(), _this->hostBuf, sizeof(_this->hostBuf))) {
            _this->tcpHost = _this->hostBuf;
            _this->saveConfig();
        }
        ImGui::LeftLabel("Port");
        ImGui::FillWidth();
        if (ImGui::InputInt(("##adsb_port_" + _this->name).c_str(), &_this->tcpPort)) {
            if (_this->tcpPort < 1) _this->tcpPort = 1;
            if (_this->tcpPort > 65535) _this->tcpPort = 65535;
            _this->saveConfig();
        }
        if (ImGui::Checkbox(("Enable TCP##adsb_tcp_en_" + _this->name).c_str(), &_this->tcpEnabled)) {
            if (_this->tcpEnabled) { _this->tcp.start(_this->tcpHost, _this->tcpPort); }
            else { _this->tcp.stop(); }
            _this->saveConfig();
        }
        ImGui::SameLine();
        if (_this->tcpEnabled && _this->tcp.isConnected()) {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "connected");
        } else if (_this->tcpEnabled) {
            ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.1f, 1.0f), "connecting...");
        } else {
            ImGui::TextDisabled("disabled");
        }

        ImGui::Separator();

        // --- Reference position (optional, for single-frame local decode) ---
        if (ImGui::Checkbox(("Use reference position##adsb_useref_" + _this->name).c_str(), &_this->useRef)) {
            _this->saveConfig();
        }
        if (_this->useRef) {
            ImGui::LeftLabel("Ref lat");
            ImGui::FillWidth();
            if (ImGui::InputDouble(("##adsb_reflat_" + _this->name).c_str(), &_this->refLat, 0.0, 0.0, "%.5f")) { _this->saveConfig(); }
            ImGui::LeftLabel("Ref lon");
            ImGui::FillWidth();
            if (ImGui::InputDouble(("##adsb_reflon_" + _this->name).c_str(), &_this->refLon, 0.0, 0.0, "%.5f")) { _this->saveConfig(); }
        }

        ImGui::Separator();

        // --- Stats ---
        uint64_t frames = _this->decoder ? _this->decoder->getFrameCount() : 0;
        size_t nac = 0, npos = 0;
        {
            std::lock_guard<std::mutex> lck(_this->acMtx);
            _this->pruneAircraft(300.0); // forget after 5 min
            nac = _this->aircraft.size();
            for (auto& kv : _this->aircraft) { if (kv.second.havePos) npos++; }
        }
        ImGui::Text("Frames (CRC OK): %llu", (unsigned long long)frames);
        ImGui::Text("Aircraft tracked: %zu  (positioned: %zu)", nac, npos);

        ImGui::Separator();

        // --- Aircraft table: in its own window so it can be moved/resized ---
        if (ImGui::Button(("Open aircraft window##adsb_openwin_" + _this->name).c_str(), ImVec2(menuWidth, 0))) {
            _this->showWindow = true;
        }

        if (!_this->enabled) { style::endDisabled(); }

        // Draw the separate window (outside the disabled block so it stays usable).
        if (_this->showWindow) { _this->drawAircraftWindow(); }
    }

    // Separate, movable/resizable window holding the decoded-aircraft table.
    // Rows are kept in first-seen order (a given aircraft never jumps rows).
    void drawAircraftWindow() {
        std::string title = "ADS-B Aircraft (" + name + ")###adsb_win_" + name;
        ImGui::SetNextWindowSize(ImVec2(760, 420), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(title.c_str(), &showWindow)) {
            ImGui::End();
            return;
        }

        // Prevent SDR++'s waterfall from reacting to clicks/drags that originate
        // inside this window. The waterfall uses raw mouse state plus a geometric
        // hit test that ignores overlapping ImGui windows, so dragging this window
        // over the waterfall would otherwise move the VFO. We assert the public
        // lock flag (reset to false at the start of every frame by MainWindow)
        // whenever our window is hovered OR being actively dragged.
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                                   ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
            gui::mainWindow.lockWaterfallControls = true;
        }

        // Snapshot rows in stable first-seen order.
        double t = nowSec();
        std::vector<Aircraft> rows;
        {
            std::lock_guard<std::mutex> lck(acMtx);
            rows.reserve(order.size());
            for (uint32_t icao : order) {
                auto it = aircraft.find(icao);
                if (it != aircraft.end()) { rows.push_back(it->second); }
            }
        }

        ImGui::Text("Aircraft tracked: %zu", rows.size());
        if (ImGui::BeginTable(("##adsb_table_" + name).c_str(), 9,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                              ImGuiTableFlags_SizingStretchProp,
                              ImVec2(0, 0))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("ICAO");
            ImGui::TableSetupColumn("Callsign");
            ImGui::TableSetupColumn("Cat");
            ImGui::TableSetupColumn("Lat");
            ImGui::TableSetupColumn("Lon");
            ImGui::TableSetupColumn("Alt(ft)");
            ImGui::TableSetupColumn("Spd(kt)");
            ImGui::TableSetupColumn("Hdg");
            ImGui::TableSetupColumn("Age(s)");
            ImGui::TableHeadersRow();

            for (auto& ac : rows) {
                ImGui::TableNextRow();
                char icaoStr[8]; std::snprintf(icaoStr, sizeof(icaoStr), "%06X", ac.icao);
                ImGui::TableSetColumnIndex(0); ImGui::Text("%s", icaoStr);
                ImGui::TableSetColumnIndex(1); ImGui::Text("%s", ac.callsign.c_str());
                ImGui::TableSetColumnIndex(2); if (!ac.category.empty()) ImGui::Text("%s", ac.category.c_str()); else ImGui::TextDisabled("-");
                ImGui::TableSetColumnIndex(3); if (ac.havePos) ImGui::Text("%.4f", ac.lat); else ImGui::TextDisabled("-");
                ImGui::TableSetColumnIndex(4); if (ac.havePos) ImGui::Text("%.4f", ac.lon); else ImGui::TextDisabled("-");
                ImGui::TableSetColumnIndex(5); if (ac.haveAlt) ImGui::Text("%d", ac.altFt); else ImGui::TextDisabled("-");
                ImGui::TableSetColumnIndex(6); if (ac.haveSpeed) ImGui::Text("%.0f", ac.speedKt); else ImGui::TextDisabled("-");
                ImGui::TableSetColumnIndex(7); if (ac.haveHeading) ImGui::Text("%.0f", ac.headingDeg); else ImGui::TextDisabled("-");
                ImGui::TableSetColumnIndex(8); ImGui::Text("%.0f", t - ac.lastSeen);
            }
            ImGui::EndTable();
        }

        ImGui::End();
    }

    void saveConfig() {
        config.acquire();
        config.conf[name]["tcpHost"] = tcpHost;
        config.conf[name]["tcpPort"] = tcpPort;
        config.conf[name]["tcpEnabled"] = tcpEnabled;
        config.conf[name]["useRef"] = useRef;
        config.conf[name]["refLat"] = refLat;
        config.conf[name]["refLon"] = refLon;
        config.release(true);
    }

    std::string name;
    bool enabled = true;

    VFOManager::VFO* vfo = nullptr;
    std::unique_ptr<ADSBDecoder> decoder;

    // Aircraft tracking.
    std::mutex acMtx;
    std::map<uint32_t, Aircraft> aircraft;
    std::vector<uint32_t> order;   // ICAO in first-seen order (stable row layout)
    bool showWindow = false;       // separate aircraft-table window visibility

    // TCP output.
    TCPSender tcp;
    std::string tcpHost = "127.0.0.1";
    int tcpPort = 10100;
    bool tcpEnabled = false;
    char hostBuf[128] = "127.0.0.1";

    // Reference position (optional).
    bool useRef = false;
    double refLat = 43.7;
    double refLon = 7.25;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/adsb_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new ADSBDecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (ADSBDecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
