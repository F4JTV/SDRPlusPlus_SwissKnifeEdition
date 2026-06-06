/** @file
    SDR++ rtl_433 bridge module.

    Links the real rtl_433 library and feeds it IQ from an SDR++ VFO, giving
    access to all ~320 rtl_433 device decoders inside SDR++. Decoded messages
    are shown in a live table.

    Built on SDR++ (https://github.com/AlexandreRouma/SDRPlusPlus, GPLv3) and
    rtl_433 (https://github.com/merbanan/rtl_433, GPLv2+).
    Licensed under the GNU General Public License v3 or later.
*/
#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <utils/flog.h>
#include <ctime>
#include <deque>
#include <mutex>
#include <algorithm>
#include "rtl433_bridge_dsp.h"

#define CONCAT(a, b) ((std::string(a) + b).c_str())

// rtl_433 decimates the SDR to 250 kHz for OOK/most FSK protocols.
#define RTL433_SAMPLERATE 250000

SDRPP_MOD_INFO{
    /* Name:            */ "rtl_433_bridge",
    /* Description:     */ "Full rtl_433 ISM decoder (links librtl_433, ~320 protocols)",
    /* Author:          */ "F4JTV",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

ConfigManager config;

// Sanitize the version string returned by librtl_433.
//
// When the rtl_433 source tree was built without accessible git tags (a shallow
// clone, a fork without --tags fetched, etc.), CMake fails to determine the
// version and substitutes "NOTFOUND" into the generated version.h. The library
// then returns a string like:
//   "rtl_433 version -128-NOTFOUND branch master at ... inputs ..."
//
// We detect that case and replace the malformed version token with "(unknown)"
// so the UI shows e.g.:
//   "rtl_433 version (unknown) branch master at ... inputs ..."
// regardless of how the upstream tree was cloned. Purely cosmetic; the decode
// pipeline is unaffected.
static std::string sanitizeVersion(const std::string& raw) {
    auto notFoundPos = raw.find("NOTFOUND");
    if (notFoundPos == std::string::npos) { return raw; }
    const std::string head = "version ";
    auto verStart = raw.find(head);
    if (verStart == std::string::npos) { return raw; }
    auto tokStart = verStart + head.size();
    auto tokEnd = raw.find(' ', tokStart);
    if (tokEnd == std::string::npos) { tokEnd = raw.size(); }
    // Only sanitize if NOTFOUND is actually inside the version token, to
    // avoid corrupting strings where NOTFOUND happens to appear elsewhere.
    if (notFoundPos < tokStart || notFoundPos >= tokEnd) { return raw; }
    std::string v = raw;
    v.replace(tokStart, tokEnd - tokStart, "(unknown)");
    return v;
}

class RTL433BridgeModule : public ModuleManager::Instance {
public:
    RTL433BridgeModule(std::string name) {
        this->name = name;

        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0,
                                            RTL433_SAMPLERATE, RTL433_SAMPLERATE,
                                            RTL433_SAMPLERATE, RTL433_SAMPLERATE, true);
        vfo->setSnapInterval(1000);

        uint32_t center = (uint32_t)gui::waterfall.getCenterFrequency();
        dsp.init(vfo->output, RTL433_SAMPLERATE, center,
                 [this](const rtl433br::Message& m) { onMessage(m); });
        dsp.setGain(gain);
        dsp.setLevels(minLevelDb, minSnrDb);

        protoCount = dsp.getRadio().protocolCount();
        rtlVersion = sanitizeVersion(rtl433br::RTL433::version());

        dsp.start();
        enabled = true;

        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~RTL433BridgeModule() {
        gui::menu.removeEntry(name);
        if (enabled) {
            dsp.stop();
            sigpath::vfoManager.deleteVFO(vfo);
        }
        sigpath::sinkManager.unregisterStream(name);
    }

    void postInit() {}

    void enable() {
        double bw = gui::waterfall.getBandwidth();
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER,
                                            std::clamp<double>(0, -bw / 2.0, bw / 2.0),
                                            RTL433_SAMPLERATE, RTL433_SAMPLERATE,
                                            RTL433_SAMPLERATE, RTL433_SAMPLERATE, true);
        vfo->setSnapInterval(1000);
        dsp.setInput(vfo->output);
        dsp.start();
        enabled = true;
    }

    void disable() {
        dsp.stop();
        sigpath::vfoManager.deleteVFO(vfo);
        enabled = false;
    }

    bool isEnabled() { return enabled; }

private:
    struct LogEntry {
        std::string time;
        std::string model;
        std::string fields;
    };

    void onMessage(const rtl433br::Message& m) {
        LogEntry e;
        char tbuf[16];
        time_t now = time(nullptr);
        strftime(tbuf, sizeof(tbuf), "%H:%M:%S", localtime(&now));
        e.time = tbuf;
        e.model = m.model;
        std::string f;
        for (size_t i = 0; i < m.fields.size(); ++i) {
            // The library already includes a "time" field; skip it in the flat view.
            if (m.fields[i].key == "time") { continue; }
            if (!f.empty()) { f += "  "; }
            f += m.fields[i].key + "=" + m.fields[i].value;
        }
        e.fields = f;

        std::lock_guard<std::mutex> lck(logMtx);
        log.push_back(e);
        if (log.size() > 1000) { log.pop_front(); }
        msgCount++;
    }

    static void menuHandler(void* ctx) {
        RTL433BridgeModule* _this = (RTL433BridgeModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) { style::beginDisabled(); }

        ImGui::TextWrapped("%s", _this->rtlVersion.c_str());
        ImGui::Text("Protocols loaded: %d", _this->protoCount);

        ImGui::LeftLabel("Gain");
        ImGui::FillWidth();
        if (ImGui::SliderFloat(CONCAT("##rtl433br_gain_", _this->name), &_this->gain, 0.1f, 50.0f, "%.1f")) {
            _this->dsp.setGain(_this->gain);
        }

        ImGui::LeftLabel("Min level (dB)");
        ImGui::FillWidth();
        if (ImGui::SliderFloat(CONCAT("##rtl433br_minlvl_", _this->name), &_this->minLevelDb, -30.0f, 0.0f, "%.1f")) {
            _this->dsp.setLevels(_this->minLevelDb, _this->minSnrDb);
        }

        ImGui::LeftLabel("SNR (dB)");
        ImGui::FillWidth();
        if (ImGui::SliderFloat(CONCAT("##rtl433br_snr_", _this->name), &_this->minSnrDb, 3.0f, 20.0f, "%.1f")) {
            _this->dsp.setLevels(_this->minLevelDb, _this->minSnrDb);
        }

        ImGui::Text("Messages decoded: %d", _this->msgCount);

        // Toggle the separate messages window.
        if (_this->showMessages) {
            if (ImGui::Button(CONCAT("Hide messages##rtl433br_toggle_", _this->name), ImVec2(menuWidth, 0))) {
                _this->showMessages = false;
            }
        }
        else {
            if (ImGui::Button(CONCAT("Show messages##rtl433br_toggle_", _this->name), ImVec2(menuWidth, 0))) {
                _this->showMessages = true;
            }
        }

        if (!_this->enabled) { style::endDisabled(); }

        // --- Separate, floating messages window with autoscroll ------------
        // Structure mirrors the working POCSAG module: the TABLE itself is the
        // scrolling region (ImGuiTableFlags_ScrollY), and SetScrollHereY is
        // called inside the table just before EndTable so it acts on the
        // table's own scroll context.
        if (_this->showMessages) {
            // Prevent the waterfall/VFO from reacting to drags on this window.
            gui::mainWindow.lockWaterfallControls = true;

            std::string title = "rtl_433 messages (" + _this->name + ")###rtl433br_win_" + _this->name;
            ImGui::SetNextWindowSize(ImVec2(700.0f, 400.0f), ImGuiCond_FirstUseEver);
            if (!ImGui::Begin(title.c_str(), &_this->showMessages)) {
                ImGui::End();
            }
            else {
                // Toolbar
                if (ImGui::Button(CONCAT("Clear##rtl433br_clear_", _this->name))) {
                    std::lock_guard<std::mutex> lck(_this->logMtx);
                    _this->log.clear();
                    _this->msgCount = 0;
                }
                ImGui::SameLine();
                ImGui::Checkbox(CONCAT("Auto-scroll##rtl433br_auto_", _this->name), &_this->autoScroll);
                ImGui::SameLine();
                ImGui::Text("(%d shown)", (int)_this->log.size());

                const ImGuiTableFlags tableFlags =
                    ImGuiTableFlags_Borders |
                    ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_Resizable |
                    ImGuiTableFlags_ScrollX |
                    ImGuiTableFlags_ScrollY;

                if (ImGui::BeginTable(CONCAT("##rtl433br_tbl_", _this->name), 3, tableFlags,
                                      ImVec2(0.0f, 0.0f))) {
                    ImGui::TableSetupScrollFreeze(0, 1); // keep header visible
                    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                    ImGui::TableSetupColumn("Model", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                    ImGui::TableSetupColumn("Data", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();

                    {
                        std::lock_guard<std::mutex> lck(_this->logMtx);
                        for (auto& e : _this->log) {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(e.time.c_str());
                            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(e.model.c_str());
                            ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(e.fields.c_str());
                        }
                    }

                    // Stick to the bottom when new messages arrive (POCSAG pattern).
                    if (_this->autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10.0f) {
                        ImGui::SetScrollHereY(1.0f);
                    }

                    ImGui::EndTable();
                }

                ImGui::End();
            }
        }
    }

    std::string name;
    bool enabled = false;

    VFOManager::VFO* vfo = nullptr;
    RTL433BridgeDSP dsp;

    float gain = 4.0f;
    float minLevelDb = -12.1442f;
    float minSnrDb = 9.0f;
    int protoCount = 0;
    std::string rtlVersion;

    bool showMessages = true;
    bool autoScroll = true;

    std::mutex logMtx;
    std::deque<LogEntry> log;
    int msgCount = 0;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/rtl_433_bridge_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new RTL433BridgeModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (RTL433BridgeModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
