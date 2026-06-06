#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <gui/tuner.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <utils/flog.h>

#include <time.h>
#include <stdio.h>
#include <vector>
#include <deque>
#include <string>
#include <mutex>
#include <cstring>

#include "dsp.h"
#include "aprs/afsk.h"
#include "aprs/ax25.h"
#include "aprs/symbols.h"
#include "tcp_sender.h"

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "aprs_decoder",
    /* Description:     */ "APRS (AX.25 AFSK 1200) decoder with TCP map output",
    /* Author:          */ "F4JTV (ADRASEC 06)",
    /* Version:         */ 1, 0, 0,
    /* Max instances    */ -1
};

ConfigManager config;

// Common APRS frequencies (MHz)
static constexpr double APRS_FREQ_EU = 144.800e6; // IARU Region 1
static constexpr double APRS_FREQ_US = 144.390e6; // North America

// One decoded packet, as displayed in the separate window.
struct DisplayPacket {
    std::string time;     // "HH:MM:SS" (UTC)
    std::string date;     // "YYYY-MM-DD" (UTC)
    std::string name;     // station callsign or object/item name
    std::string source;   // AX.25 source callsign
    std::string typeDesc; // "Position", "MIC-E", "Object", ...
    bool   hasPos = false;
    double lat = 0, lon = 0;
    bool   hasSpeed = false;
    double speed = 0;
    bool   hasCourse = false;
    int    course = 0;
    bool   hasAlt = false;
    double altM = 0;      // altitude in metres
    char   symTable = '/';
    char   symCode = '>';
    std::string path;     // digipeater path, joined
    std::string comment;  // free-form remainder

    // ---- weather (metric units, converted from raw APRS) -------------------
    bool   isWeather = false;
    bool   hasTemp = false;     double tempC = 0;       // deg C
    bool   hasHum = false;      int    humidity = 0;    // %
    bool   hasWindDir = false;  int    windDir = 0;     // deg
    bool   hasWind = false;     double windKmh = 0;     // km/h
    bool   hasGust = false;     double gustKmh = 0;     // km/h
    bool   hasPressure = false; double pressureHpa = 0; // hPa
    bool   hasRain1h = false;   double rain1hMm = 0;    // mm
};

class APRSDecoderModule : public ModuleManager::Instance {
public:
    APRSDecoderModule(std::string name) {
        this->name = name;

        // Load persisted settings
        config.acquire();
        if (config.conf[name].contains("tcpHost"))    { tcpHost = config.conf[name]["tcpHost"]; }
        if (config.conf[name].contains("tcpPort"))    { tcpPort = config.conf[name]["tcpPort"]; }
        if (config.conf[name].contains("tcpEnabled")) { tcpEnabled = config.conf[name]["tcpEnabled"]; }
        if (config.conf[name].contains("showWindow")) { showWindow = config.conf[name]["showWindow"]; }
        if (config.conf[name].contains("autoScroll")) { autoScroll = config.conf[name]["autoScroll"]; }
        if (config.conf[name].contains("showSymbols")){ showSymbols = config.conf[name]["showSymbols"]; }
        if (config.conf[name].contains("snapInterval")){ snapInterval = config.conf[name]["snapInterval"]; }
        config.release();
        strncpy(hostBuf, tcpHost.c_str(), sizeof(hostBuf) - 1);

        // Create the VFO: 12.5 kHz channel, 24 kHz IQ (== 20 samples/bit)
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER,
                                            0, 12500, 24000, 12500, 12500, true);
        vfo->setSnapInterval(snapInterval);

        // Build the DSP chain
        afsk.setFrameHandler([this](const uint8_t* f, int len) { this->onFrame(f, len); });
        dsp.init(vfo->output, 24000);
        audioSink.init(&dsp.out, _audioHandler, this);

        // TCP
        tcp.configure(tcpHost, tcpPort);
        if (tcpEnabled) { tcp.start(); }

        // Start DSP
        dsp.start();
        audioSink.start();

        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~APRSDecoderModule() {
        gui::menu.removeEntry(name);
        if (enabled) {
            audioSink.stop();
            dsp.stop();
            sigpath::vfoManager.deleteVFO(vfo);
        }
        tcp.stop();
        sigpath::sinkManager.unregisterStream(name);
    }

    void postInit() {}

    void enable() {
        double bw = gui::waterfall.getBandwidth();
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER,
                                            std::clamp<double>(0, -bw / 2.0, bw / 2.0),
                                            12500, 24000, 12500, 12500, true);
        vfo->setSnapInterval(snapInterval);
        dsp.setInput(vfo->output);
        dsp.start();
        audioSink.start();
        enabled = true;
    }

    void disable() {
        audioSink.stop();
        dsp.stop();
        sigpath::vfoManager.deleteVFO(vfo);
        vfo = NULL;
        enabled = false;
    }

    bool isEnabled() { return enabled; }

private:
    // Audio handler (sink worker thread): forward FM audio to the AFSK decoder.
    static void _audioHandler(float* data, int count, void* ctx) {
        APRSDecoderModule* _this = (APRSDecoderModule*)ctx;
        _this->afsk.process(data, count);
    }

    // Called (sink thread) for every FCS-valid AX.25 frame.
    void onFrame(const uint8_t* f, int len) {
        aprs::AX25Frame fr;
        if (!aprs::parseAX25(f, len, fr)) { return; }
        aprs::APRSRecord r = aprs::parseAPRS(fr);

        // UTC timestamp
        time_t now = time(NULL);
        struct tm tmv;
#ifdef _WIN32
        gmtime_s(&tmv, &now);
#else
        gmtime_r(&now, &tmv);
#endif
        char dbuf[16], hbuf[16];
        strftime(dbuf, sizeof(dbuf), "%Y-%m-%d", &tmv);
        strftime(hbuf, sizeof(hbuf), "%H:%M:%S", &tmv);

        // Build the display packet
        DisplayPacket p;
        p.date = dbuf; p.time = hbuf;
        p.name = r.name.empty() ? fr.source : r.name;
        p.source = fr.source;
        p.typeDesc = r.typeDesc;
        p.hasPos = r.hasPosition;
        p.lat = r.lat; p.lon = r.lon;
        p.hasSpeed = r.hasSpeed; p.speed = r.speedKnots;
        p.hasCourse = r.hasCourse; p.course = r.course;
        p.hasAlt = r.hasAltitude; p.altM = r.altitudeM;
        p.symTable = r.symbolTable; p.symCode = r.symbolCode;
        p.comment = r.comment;

        // Weather: convert raw APRS units to metric for display/JSON.
        p.isWeather = r.hasWeather;
        if (r.hasTemp)     { p.hasTemp = true;     p.tempC = (r.tempF - 32.0) * 5.0 / 9.0; }
        if (r.hasHumidity) { p.hasHum = true;      p.humidity = r.humidity; }
        if (r.hasWindDir)  { p.hasWindDir = true;  p.windDir = r.windDir; }
        if (r.hasWindSpd)  { p.hasWind = true;     p.windKmh = r.windSpdMph * 1.609344; }
        if (r.hasGust)     { p.hasGust = true;     p.gustKmh = r.gustMph * 1.609344; }
        if (r.hasPressure) { p.hasPressure = true; p.pressureHpa = r.pressureHpa; }
        if (r.hasRain1h)   { p.hasRain1h = true;   p.rain1hMm = r.rain1hIn * 25.4; }

        std::string path;
        for (size_t i = 0; i < fr.path.size(); i++) {
            if (i) { path += ","; }
            path += fr.path[i];
        }
        p.path = path;

        {
            std::lock_guard<std::mutex> lck(pktMtx);
            packets.push_back(p);
            while (packets.size() > MAX_PACKETS) { packets.pop_front(); }
        }

        // Forward to TCP only if positioned (mirrors the AIS module behaviour).
        // Weather stations are sent with type "APRS Meteo", everything else
        // positioned with type "APRS".
        if (tcpEnabled && p.hasPos) {
            tcp.send(buildJson(p));
        }
    }

    static std::string jsonEscape(const std::string& s) {
        std::string o;
        for (char c : s) {
            switch (c) {
                case '"':  o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n"; break;
                case '\r': o += "\\r"; break;
                case '\t': o += "\\t"; break;
                default:
                    if ((unsigned char)c < 0x20) { /* drop control chars */ }
                    else { o += c; }
            }
        }
        return o;
    }

    static std::string buildJson(const DisplayPacket& p) {
        // Compose a readable "info" field: type, symbol, course, path, comment.
        std::string info = p.typeDesc;
        char sym[8]; snprintf(sym, sizeof(sym), " sym=%c%c", p.symTable, p.symCode);
        info += sym;
        if (p.hasCourse) { info += " crs=" + std::to_string(p.course); }
        if (!p.path.empty()) { info += " via " + p.path; }
        if (!p.comment.empty()) { info += " - " + p.comment; }

        // Dedicated "symbol" field for the map: the 2-char APRS symbol = table
        // identifier ('/', '\\' or an overlay char) followed by the symbol code.
        // Escaped because the alternate-table identifier is a backslash.
        std::string symbol;
        symbol += p.symTable;
        symbol += p.symCode;

        // ---- weather station: type "APRS Meteo" with metric weather fields --
        if (p.isWeather) {
            std::string fields;
            char fb[64];
            if (p.hasTemp)     { snprintf(fb, sizeof(fb), ",\"temp_c\":%.1f", p.tempC);            fields += fb; }
            else               { fields += ",\"temp_c\":null"; }
            if (p.hasHum)      { snprintf(fb, sizeof(fb), ",\"humidity\":%d", p.humidity);         fields += fb; }
            else               { fields += ",\"humidity\":null"; }
            if (p.hasWindDir)  { snprintf(fb, sizeof(fb), ",\"wind_dir\":%d", p.windDir);          fields += fb; }
            else               { fields += ",\"wind_dir\":null"; }
            if (p.hasWind)     { snprintf(fb, sizeof(fb), ",\"wind_kmh\":%.1f", p.windKmh);        fields += fb; }
            else               { fields += ",\"wind_kmh\":null"; }
            if (p.hasGust)     { snprintf(fb, sizeof(fb), ",\"gust_kmh\":%.1f", p.gustKmh);        fields += fb; }
            else               { fields += ",\"gust_kmh\":null"; }
            if (p.hasPressure) { snprintf(fb, sizeof(fb), ",\"pressure_hpa\":%.1f", p.pressureHpa);fields += fb; }
            else               { fields += ",\"pressure_hpa\":null"; }
            if (p.hasRain1h)   { snprintf(fb, sizeof(fb), ",\"rain_mm_1h\":%.1f", p.rain1hMm);     fields += fb; }
            else               { fields += ",\"rain_mm_1h\":null"; }

            std::string latlon;
            char lb[48];
            if (p.hasPos) { snprintf(lb, sizeof(lb), "\"lat\":%.6f,\"lon\":%.6f", p.lat, p.lon); latlon = lb; }
            else          { latlon = "\"lat\":null,\"lon\":null"; }

            std::string out = "{\"name\":\"" + jsonEscape(p.name) + "\",\"date\":\"" + p.date +
                "\",\"time\":\"" + p.time + "\"," + latlon +
                ",\"type\":\"APRS Meteo\",\"symbol\":\"" + jsonEscape(symbol) + "\"" +
                fields + ",\"info\":\"" + jsonEscape(info) + "\"}";
            return out;
        }

        // ---- position / other traffic: type "APRS" -------------------------
        std::string speedStr = p.hasSpeed ? [&]{ char b[32]; snprintf(b, sizeof(b), "%.1f", p.speed); return std::string(b); }() : std::string("null");
        std::string courseStr = p.hasCourse ? std::to_string(p.course) : std::string("null");
        std::string altStr = p.hasAlt ? [&]{ char b[32]; snprintf(b, sizeof(b), "%.0f", p.altM); return std::string(b); }() : std::string("null");

        std::string out = "{\"name\":\"" + jsonEscape(p.name) + "\",\"date\":\"" + p.date +
            "\",\"time\":\"" + p.time + "\",";
        { char ll[48]; snprintf(ll, sizeof(ll), "\"lat\":%.6f,\"lon\":%.6f,", p.lat, p.lon); out += ll; }
        out += "\"type\":\"APRS\",\"symbol\":\"" + jsonEscape(symbol) + "\"," +
            "\"speed\":" + speedStr + "," +
            "\"course\":" + courseStr + "," +
            "\"altitude_m\":" + altStr + "," +
            "\"info\":\"" + jsonEscape(info) + "\"}";
        return out;
    }

    // ---- GUI ---------------------------------------------------------------

    static void menuHandler(void* ctx) {
        APRSDecoderModule* _this = (APRSDecoderModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) { style::beginDisabled(); }

        // Quick-tune buttons (set the actual radio center frequency)
        ImGui::TextUnformatted("Quick tune:");
        if (ImGui::Button(CONCAT("144.800 (EU)##aprs_eu_", _this->name),
                          ImVec2(menuWidth / 2.0f - 4, 0))) {
            tuner::tune(tuner::TUNER_MODE_NORMAL, _this->name, APRS_FREQ_EU);
        }
        ImGui::SameLine();
        if (ImGui::Button(CONCAT("144.390 (US)##aprs_us_", _this->name),
                          ImVec2(menuWidth / 2.0f - 4, 0))) {
            tuner::tune(tuner::TUNER_MODE_NORMAL, _this->name, APRS_FREQ_US);
        }

        // Snap interval
        ImGui::LeftLabel("Snap");
        ImGui::FillWidth();
        if (ImGui::InputInt(CONCAT("##aprs_snap_", _this->name), &_this->snapInterval, 100, 1000)) {
            if (_this->snapInterval < 1) { _this->snapInterval = 1; }
            if (_this->vfo) { _this->vfo->setSnapInterval(_this->snapInterval); }
            _this->saveConfig();
        }

        ImGui::Separator();

        // TCP output configuration
        ImGui::TextUnformatted("TCP map output");
        ImGui::LeftLabel("Host");
        ImGui::FillWidth();
        if (ImGui::InputText(CONCAT("##aprs_host_", _this->name), _this->hostBuf, sizeof(_this->hostBuf))) {
            _this->tcpHost = _this->hostBuf;
            _this->tcp.configure(_this->tcpHost, _this->tcpPort);
            _this->saveConfig();
        }
        ImGui::LeftLabel("Port");
        ImGui::FillWidth();
        if (ImGui::InputInt(CONCAT("##aprs_port_", _this->name), &_this->tcpPort, 1, 10)) {
            if (_this->tcpPort < 1) { _this->tcpPort = 1; }
            if (_this->tcpPort > 65535) { _this->tcpPort = 65535; }
            _this->tcp.configure(_this->tcpHost, _this->tcpPort);
            _this->saveConfig();
        }

        // Enable TCP checkbox + inline status label (mirrors the AIS module).
        if (ImGui::Checkbox(CONCAT("Enable TCP##aprs_tcpen_", _this->name), &_this->tcpEnabled)) {
            // Apply the state change. Both start() and stop() are idempotent
            // and non-blocking, so the GUI thread is never frozen here.
            if (_this->tcpEnabled) {
                _this->tcp.configure(_this->tcpHost, _this->tcpPort);
                _this->tcp.start();
            } else {
                _this->tcp.stop();
            }
            _this->saveConfig();
        }
        ImGui::SameLine();
        if (!_this->tcpEnabled) {
            ImGui::TextDisabled("disabled");
        } else if (_this->tcp.isConnected()) {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "connected");
        } else {
            ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "enabled");
        }
        if (_this->tcpEnabled) {
            ImGui::Text("Sent: %llu  Dropped: %llu",
                        (unsigned long long)_this->tcp.getSent(),
                        (unsigned long long)_this->tcp.getDropped());
        }

        ImGui::Separator();

        // Packets window toggle
        size_t count;
        { std::lock_guard<std::mutex> lck(_this->pktMtx); count = _this->packets.size(); }
        if (ImGui::Button(CONCAT("Show packets##aprs_show_", _this->name), ImVec2(menuWidth, 0))) {
            _this->showWindow = true;
            _this->saveConfig();
        }
        ImGui::Text("Packets: %zu", count);

        if (!_this->enabled) { style::endDisabled(); }

        // Draw the separate window (must be called every frame from here)
        _this->drawPacketsWindow();
    }

    void drawPacketsWindow() {
        if (!showWindow) { return; }

        ImGui::SetNextWindowSize(ImVec2(900, 400), ImGuiCond_FirstUseEver);
        std::string title = "APRS packets [" + name + "]###aprs_win_" + name;
        if (ImGui::Begin(title.c_str(), &showWindow)) {

            // --- anti-VFO lock: prevent the waterfall from reacting to clicks /
            // drags that originate inside this window. The waterfall input
            // handler uses raw mouse state plus a geometric hit test that
            // ignores overlapping ImGui windows, so without this, dragging this
            // window's title bar over the waterfall would move the VFO. The core
            // resets lockWaterfallControls every frame, so we just assert it
            // while our window is engaged (hovered OR focused, incl. title-bar
            // drag where the cursor may briefly leave the rect).
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows
                                       | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
                || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
                gui::mainWindow.lockWaterfallControls = true;
            }

            // Toolbar
            if (ImGui::Button(CONCAT("Clear##aprs_clear_", name))) {
                std::lock_guard<std::mutex> lck(pktMtx);
                packets.clear();
            }
            ImGui::SameLine();
            if (ImGui::Checkbox(CONCAT("Auto-scroll##aprs_as_", name), &autoScroll)) { saveConfig(); }
            if (symbols.available()) {
                ImGui::SameLine();
                if (ImGui::Checkbox(CONCAT("Show icons##aprs_sym_", name), &showSymbols)) { saveConfig(); }
            }
            ImGui::SameLine();
            {
                std::lock_guard<std::mutex> lck(pktMtx);
                ImGui::Text("(%zu shown)", packets.size());
            }

            // Two tabs: positioned/other traffic vs. weather stations.
            std::lock_guard<std::mutex> lck(pktMtx);
            if (ImGui::BeginTabBar(CONCAT("##aprs_tabs_", name))) {
                if (ImGui::BeginTabItem(CONCAT("Positions##aprs_pos_", name))) {
                    drawPositionsTable();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(CONCAT("Meteo##aprs_wx_", name))) {
                    drawWeatherTable();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();

        // Persist the close action (clicking the window's X)
        static bool prevShow = showWindow;
        if (prevShow != showWindow) { saveConfig(); prevShow = showWindow; }
    }

    // Render the APRS symbol for a packet in the current table cell: the icon
    // if the sheets loaded, otherwise the 2-char symbol as text.
    void drawSymbolCell(const DisplayPacket& p) {
        float sz = ImGui::GetTextLineHeight();
        if (showSymbols && symbols.drawSymbol(p.symTable, p.symCode, sz)) {
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%c%c", p.symTable, p.symCode);
            }
        } else {
            char s[3] = { p.symTable, p.symCode, 0 };
            ImGui::TextUnformatted(s);
        }
    }

    // Caller must hold pktMtx. Non-weather frames (positions, objects, status…).
    void drawPositionsTable() {
        ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                              | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable
                              | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable(CONCAT("##aprs_table_pos_", name), 11, flags)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Sym", ImGuiTableColumnFlags_WidthFixed, ImGui::GetTextLineHeight() + 4.0f);
            ImGui::TableSetupColumn("Callsign");
            ImGui::TableSetupColumn("Date");
            ImGui::TableSetupColumn("Time (UTC)");
            ImGui::TableSetupColumn("Lat");
            ImGui::TableSetupColumn("Lon");
            ImGui::TableSetupColumn("Speed");
            ImGui::TableSetupColumn("Course");
            ImGui::TableSetupColumn("Alt (m)");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Info");
            ImGui::TableHeadersRow();

            for (const auto& p : packets) {
                if (p.isWeather) { continue; }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); drawSymbolCell(p);
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(p.name.c_str());
                ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(p.date.c_str());
                ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(p.time.c_str());
                ImGui::TableSetColumnIndex(4);
                if (p.hasPos) { ImGui::Text("%.6f", p.lat); } else { ImGui::TextUnformatted("-"); }
                ImGui::TableSetColumnIndex(5);
                if (p.hasPos) { ImGui::Text("%.6f", p.lon); } else { ImGui::TextUnformatted("-"); }
                ImGui::TableSetColumnIndex(6);
                if (p.hasSpeed) { ImGui::Text("%.0f kn", p.speed); } else { ImGui::TextUnformatted("-"); }
                ImGui::TableSetColumnIndex(7);
                if (p.hasCourse) { ImGui::Text("%d\xC2\xB0", p.course); } else { ImGui::TextUnformatted("-"); }
                ImGui::TableSetColumnIndex(8);
                if (p.hasAlt) { ImGui::Text("%.0f", p.altM); } else { ImGui::TextUnformatted("-"); }
                ImGui::TableSetColumnIndex(9); ImGui::TextUnformatted(p.typeDesc.c_str());
                ImGui::TableSetColumnIndex(10);
                std::string detail;
                if (!p.path.empty())    { detail += "via " + p.path; }
                if (!p.comment.empty()) { if (!detail.empty()) detail += "  "; detail += p.comment; }
                ImGui::TextUnformatted(detail.c_str());
            }
            if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
                ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndTable();
        }
    }

    // Caller must hold pktMtx. Weather stations only, in metric units.
    void drawWeatherTable() {
        ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                              | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable
                              | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable(CONCAT("##aprs_table_wx_", name), 11, flags)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Sym", ImGuiTableColumnFlags_WidthFixed, ImGui::GetTextLineHeight() + 4.0f);
            ImGui::TableSetupColumn("Callsign");
            ImGui::TableSetupColumn("Time (UTC)");
            ImGui::TableSetupColumn("Lat");
            ImGui::TableSetupColumn("Lon");
            ImGui::TableSetupColumn("Temp (C)");
            ImGui::TableSetupColumn("Hum (%)");
            ImGui::TableSetupColumn("Wind (km/h)");
            ImGui::TableSetupColumn("Gust (km/h)");
            ImGui::TableSetupColumn("Press (hPa)");
            ImGui::TableSetupColumn("Rain 1h (mm)");
            ImGui::TableHeadersRow();

            for (const auto& p : packets) {
                if (!p.isWeather) { continue; }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); drawSymbolCell(p);
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(p.name.c_str());
                ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(p.time.c_str());
                ImGui::TableSetColumnIndex(3);
                if (p.hasPos) { ImGui::Text("%.6f", p.lat); } else { ImGui::TextUnformatted("-"); }
                ImGui::TableSetColumnIndex(4);
                if (p.hasPos) { ImGui::Text("%.6f", p.lon); } else { ImGui::TextUnformatted("-"); }
                ImGui::TableSetColumnIndex(5);
                if (p.hasTemp) { ImGui::Text("%.1f", p.tempC); } else { ImGui::TextUnformatted("-"); }
                ImGui::TableSetColumnIndex(6);
                if (p.hasHum) { ImGui::Text("%d", p.humidity); } else { ImGui::TextUnformatted("-"); }
                ImGui::TableSetColumnIndex(7);
                if (p.hasWind) {
                    if (p.hasWindDir) { ImGui::Text("%.0f @ %d", p.windKmh, p.windDir); }
                    else              { ImGui::Text("%.0f", p.windKmh); }
                } else { ImGui::TextUnformatted("-"); }
                ImGui::TableSetColumnIndex(8);
                if (p.hasGust) { ImGui::Text("%.0f", p.gustKmh); } else { ImGui::TextUnformatted("-"); }
                ImGui::TableSetColumnIndex(9);
                if (p.hasPressure) { ImGui::Text("%.1f", p.pressureHpa); } else { ImGui::TextUnformatted("-"); }
                ImGui::TableSetColumnIndex(10);
                if (p.hasRain1h) { ImGui::Text("%.1f", p.rain1hMm); } else { ImGui::TextUnformatted("-"); }
            }
            if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
                ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndTable();
        }
    }

    void saveConfig() {
        config.acquire();
        config.conf[name]["tcpHost"] = tcpHost;
        config.conf[name]["tcpPort"] = tcpPort;
        config.conf[name]["tcpEnabled"] = tcpEnabled;
        config.conf[name]["showWindow"] = showWindow;
        config.conf[name]["autoScroll"] = autoScroll;
        config.conf[name]["showSymbols"] = showSymbols;
        config.conf[name]["snapInterval"] = snapInterval;
        config.release(true);
    }

    // ---- members -----------------------------------------------------------
    std::string name;
    bool enabled = true;

    VFOManager::VFO* vfo = NULL;
    APRSDSP dsp;
    dsp::sink::Handler<float> audioSink;
    aprs::AFSK1200 afsk;

    TCPSender tcp;
    std::string tcpHost = "127.0.0.1";
    int  tcpPort = 10111;
    bool tcpEnabled = false;
    char hostBuf[128] = "127.0.0.1";

    int  snapInterval = 1000;
    bool showWindow = false;
    bool autoScroll = true;

    static constexpr size_t MAX_PACKETS = 2000;
    std::deque<DisplayPacket> packets;
    std::mutex pktMtx;

    aprs::SymbolSheet symbols;   // APRS icon sheets (lazy-loaded on GUI thread)
    bool showSymbols = true;     // show icon column when sheets are available
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/aprs_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new APRSDecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (APRSDecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
