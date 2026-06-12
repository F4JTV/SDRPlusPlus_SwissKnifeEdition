#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <gui/main_window.h>
#include <gui/widgets/folder_select.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <utils/optionlist.h>
#include <utils/flog.h>

#include <ctime>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <fstream>
#include <algorithm>

#include "decoder.h"
#include "pocsag/decoder.h"
#include "flex/decoder.h"

// Hard cap on the number of messages kept in memory for the GUI
#define PAGER_MAX_MESSAGES_IN_RAM   1024

SDRPP_MOD_INFO{
    /* Name:            */ "pager_decoder",
    /* Description:     */ "POCSAG and FLEX Pager Decoder",
    /* Author:          */ "SDR++ Community",
    /* Version:         */ 0, 3, 0,
    /* Max instances    */ -1
};

ConfigManager config;

// =====================================================================
// Unified message representation for the GUI table. Both POCSAG and
// FLEX decoders emit their own protocol-specific Message struct; we
// convert them to this common form on arrival.
// =====================================================================
struct UnifiedMessage {
    enum Protocol { PROTO_POCSAG, PROTO_FLEX };

    std::time_t timestamp;
    Protocol    protocol;
    int64_t     address;        // POCSAG address or FLEX capcode
    std::string typeName;       // "Alpha", "Numeric", "Tone", ...
    std::string content;
    std::string info;           // Right-hand status (FEC "+3" or "1600/2/A" for FLEX)
    bool        isToneOnly;     // Used by the "Hide tone-only" filter
    bool        hasErrors;      // Used by the "Hide errors" filter
};

// Convert a POCSAG message to the unified form
static UnifiedMessage fromPocsag(const pocsag::Message& m) {
    UnifiedMessage u;
    u.timestamp = m.timestamp;
    u.protocol  = UnifiedMessage::PROTO_POCSAG;
    u.address   = (int64_t)m.address;
    switch (m.type) {
        case pocsag::MESSAGE_TYPE_NUMERIC:      u.typeName = "Numeric"; break;
        case pocsag::MESSAGE_TYPE_ALPHANUMERIC: u.typeName = "Alpha";   break;
        case pocsag::MESSAGE_TYPE_TONE_ONLY:    u.typeName = "Tone";    break;
        default:                                u.typeName = "?";       break;
    }
    u.content    = m.content;
    u.isToneOnly = (m.type == pocsag::MESSAGE_TYPE_TONE_ONLY);
    u.hasErrors  = (m.errors > 0);

    char buf[32];
    if (m.errors > 0)         { std::snprintf(buf, sizeof(buf), "%d/%d", m.corrected, m.errors); }
    else if (m.corrected > 0) { std::snprintf(buf, sizeof(buf), "+%d", m.corrected); }
    else                      { std::snprintf(buf, sizeof(buf), "OK"); }
    u.info = buf;

    return u;
}

// Convert a FLEX message to the unified form
static UnifiedMessage fromFlex(const flex::Message& m) {
    UnifiedMessage u;
    u.timestamp = m.timestamp;
    u.protocol  = UnifiedMessage::PROTO_FLEX;
    u.address   = m.capcode;
    switch (m.type) {
        case flex::PAGE_TYPE_TONE:              u.typeName = "Tone";       break;
        case flex::PAGE_TYPE_STANDARD_NUMERIC:
        case flex::PAGE_TYPE_SPECIAL_NUMERIC:
        case flex::PAGE_TYPE_NUMBERED_NUMERIC:  u.typeName = "Numeric";    break;
        case flex::PAGE_TYPE_ALPHANUMERIC:      u.typeName = "Alpha";      break;
        case flex::PAGE_TYPE_SECURE:            u.typeName = "Secure";     break;
        case flex::PAGE_TYPE_BINARY:            u.typeName = "Binary";     break;
        case flex::PAGE_TYPE_SHORT_INSTRUCTION: u.typeName = "ShortInst";  break;
        default:                                u.typeName = "?";          break;
    }
    u.content    = m.content;
    u.isToneOnly = (m.type == flex::PAGE_TYPE_TONE);
    u.hasErrors  = false;  // FLEX BCH errors are dropped at decode time, not surfaced here

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d/%d/%c", m.baud, m.levels, m.phase);
    u.info = buf;

    return u;
}

class PagerDecoderModule : public ModuleManager::Instance {
public:
    enum Protocol { PROTO_POCSAG = 0, PROTO_FLEX = 1 };

    PagerDecoderModule(std::string name)
        : logFolderSelect("%ROOT%")
    {
        this->name = name;

        // Protocol selector options
        protocols.define(PROTO_POCSAG, "POCSAG", PROTO_POCSAG);
        protocols.define(PROTO_FLEX,   "FLEX",   PROTO_FLEX);
        protoId = protocols.keyId(PROTO_POCSAG);

        // Snap-interval options (applies to both protocols)
        snapIntervals.define(1,     "1 Hz",     1);
        snapIntervals.define(10,    "10 Hz",    10);
        snapIntervals.define(100,   "100 Hz",   100);
        snapIntervals.define(1000,  "1 kHz",    1000);
        snapIntervals.define(2500,  "2.5 kHz",  2500);
        snapIntervals.define(6250,  "6.25 kHz", 6250);
        snapIntervals.define(12500, "12.5 kHz", 12500);
        snapIntervals.define(25000, "25 kHz",   25000);
        snapId = snapIntervals.keyId(1000);

        // Build the active decoder for whatever protocol is currently selected.
        // Loads settings inside, so config-driven values are applied here.
        rebuildDecoder();

        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~PagerDecoderModule() {
        gui::menu.removeEntry(name);
        if (enabled) { tearDownDecoder(); }
    }

    void postInit() {}

    void enable() {
        if (enabled) { return; }
        rebuildDecoder();
        enabled = true;
    }

    void disable() {
        if (!enabled) { return; }
        tearDownDecoder();
        enabled = false;
    }

    bool isEnabled() { return enabled; }

private:
    // -----------------------------------------------------------------
    // Decoder lifecycle - the underlying VFO and Decoder change when
    // the user picks a different protocol, since each protocol expects
    // a different VFO sample rate and bandwidth.
    // -----------------------------------------------------------------
    void rebuildDecoder() {
        tearDownDecoder();

        // Defaults; the per-protocol Decoder applies its own bandwidth/
        // sample-rate limits in its constructor. We just need a placeholder
        // here that the decoder can rewrite.
        double bw = gui::waterfall.getBandwidth();
        vfo = sigpath::vfoManager.createVFO(name,
                                            ImGui::WaterfallVFO::REF_CENTER,
                                            std::clamp<double>(0, -bw / 2.0, bw / 2.0),
                                            12500, 24000, 12500, 12500, true);

        // Apply snap interval now so the VFO has the right step from the start
        vfo->setSnapInterval(snapIntervals.value(snapId));

        Protocol p = protocols.value(protoId);
        if (p == PROTO_POCSAG) {
            pocsagDecoder = std::make_unique<POCSAGDecoder>(name, vfo,
                [this](const pocsag::Message& m) {
                    this->onMessageReceived(fromPocsag(m));
                });
            pocsagDecoder->onSettingsChanged([this]() { this->saveSettings(); });
            flexDecoder.reset();
            activeDecoder = pocsagDecoder.get();
        } else {
            flexDecoder = std::make_unique<FLEXDecoder>(name, vfo,
                [this](const flex::Message& m) {
                    this->onMessageReceived(fromFlex(m));
                });
            pocsagDecoder.reset();
            activeDecoder = flexDecoder.get();
        }

        loadSettings();
        activeDecoder->start();
    }

    void tearDownDecoder() {
        if (activeDecoder) {
            activeDecoder->stop();
            activeDecoder = nullptr;
        }
        pocsagDecoder.reset();
        flexDecoder.reset();
        if (vfo) {
            sigpath::vfoManager.deleteVFO(vfo);
            vfo = nullptr;
        }
    }

    // -----------------------------------------------------------------
    // Settings persistence
    // -----------------------------------------------------------------
    void loadSettings() {
        config.acquire();
        if (!config.conf.contains(name)) {
            config.conf[name] = json({});
        }
        json& c = config.conf[name];

        if (c.contains("protocol")) {
            Protocol p = (Protocol)c["protocol"].get<int>();
            if (protocols.keyExists(p)) { protoId = protocols.keyId(p); }
        }

        // POCSAG-specific settings
        if (pocsagDecoder) {
            if (c.contains("baudrate")) {
                pocsagDecoder->setBaudrateFromConfig(c["baudrate"].get<int>());
            }
            if (c.contains("decodeMode")) {
                pocsagDecoder->setDecodeModeFromConfig(c["decodeMode"].get<int>());
            }
            if (c.contains("invert")) {
                pocsagDecoder->setInvertedFromConfig(c["invert"].get<bool>());
            }
            if (c.contains("lowPass")) {
                pocsagDecoder->setLowPassFromConfig(c["lowPass"].get<bool>());
            }
        }

        // Common settings
        if (c.contains("snapInterval")) {
            int snap = c["snapInterval"].get<int>();
            if (snapIntervals.keyExists(snap)) {
                snapId = snapIntervals.keyId(snap);
                if (vfo) { vfo->setSnapInterval(snap); }
            }
        }
        if (c.contains("logToFile"))         { logToFile         = c["logToFile"].get<bool>(); }
        if (c.contains("logFolder"))         { logFolderSelect.setPath(c["logFolder"].get<std::string>()); }
        if (c.contains("hideErrorMessages")) { hideErrorMessages = c["hideErrorMessages"].get<bool>(); }
        if (c.contains("hideToneOnly"))      { hideToneOnly      = c["hideToneOnly"].get<bool>(); }

        config.release();
    }

    void saveSettings() {
        config.acquire();
        if (!config.conf.contains(name)) {
            config.conf[name] = json({});
        }
        config.conf[name]["protocol"]          = (int)protocols.value(protoId);
        if (pocsagDecoder) {
            config.conf[name]["baudrate"]      = pocsagDecoder->getBaudrate();
            config.conf[name]["decodeMode"]    = pocsagDecoder->getDecodeMode();
            config.conf[name]["invert"]        = pocsagDecoder->getInverted();
            config.conf[name]["lowPass"]       = pocsagDecoder->getLowPass();
        }
        config.conf[name]["snapInterval"]      = snapIntervals.value(snapId);
        config.conf[name]["logToFile"]         = logToFile;
        config.conf[name]["logFolder"]         = logFolderSelect.path;
        config.conf[name]["hideErrorMessages"] = hideErrorMessages;
        config.conf[name]["hideToneOnly"]      = hideToneOnly;
        config.release(true);
    }

    // -----------------------------------------------------------------
    // Message handling
    // -----------------------------------------------------------------
    void onMessageReceived(const UnifiedMessage& m) {
        {
            std::lock_guard<std::mutex> lck(messagesMtx);
            messages.push_back(m);
            while (messages.size() > PAGER_MAX_MESSAGES_IN_RAM) {
                messages.pop_front();
            }
        }
        if (logToFile) { appendToLogFile(m); }
    }

    static std::string formatTimestamp(std::time_t t) {
        std::tm tm_buf;
#ifdef _WIN32
        localtime_s(&tm_buf, &t);
#else
        localtime_r(&t, &tm_buf);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
        return buf;
    }

    static const char* protoName(UnifiedMessage::Protocol p) {
        return (p == UnifiedMessage::PROTO_POCSAG) ? "POCSAG" : "FLEX";
    }

    std::string logFilePath() {
        if (!logFolderSelect.pathIsValid() || logFolderSelect.path.empty()) {
            return "";
        }
        return logFolderSelect.expandString(logFolderSelect.path) + "/pager_log.tsv";
    }

    void appendToLogFile(const UnifiedMessage& m) {
        std::string fp = logFilePath();
        if (fp.empty()) { return; }
        if (hideErrorMessages && m.hasErrors)  { return; }
        if (hideToneOnly      && m.isToneOnly) { return; }
        std::ofstream f(fp, std::ios::app);
        if (!f.is_open()) { return; }
        f << formatTimestamp(m.timestamp)
          << '\t' << protoName(m.protocol)
          << '\t' << m.address
          << '\t' << m.typeName
          << '\t' << m.info
          << '\t' << m.content
          << '\n';
    }

    // -----------------------------------------------------------------
    // GUI
    // -----------------------------------------------------------------
    static void menuHandler(void* ctx) {
        PagerDecoderModule* _this = (PagerDecoderModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) { style::beginDisabled(); }

        // Protocol selector
        ImGui::LeftLabel("Protocol");
        ImGui::FillWidth();
        if (ImGui::Combo(("##pager_decoder_proto_" + _this->name).c_str(),
                         &_this->protoId, _this->protocols.txt))
        {
            _this->saveSettings();
            _this->rebuildDecoder();
        }

        // Snap interval (applies to both protocols)
        ImGui::LeftLabel("Snap");
        ImGui::FillWidth();
        if (ImGui::Combo(("##pager_decoder_snap_" + _this->name).c_str(),
                         &_this->snapId, _this->snapIntervals.txt))
        {
            if (_this->vfo) {
                _this->vfo->setSnapInterval(_this->snapIntervals.value(_this->snapId));
            }
            _this->saveSettings();
        }

        // Per-protocol menu
        if (_this->activeDecoder) { _this->activeDecoder->showMenu(); }

        ImGui::Separator();

        if (ImGui::Button(("Show Messages##pager_decoder_show_" + _this->name).c_str(),
                          ImVec2(menuWidth, 0)))
        {
            _this->showMessagesWindow = true;
        }

        if (ImGui::Checkbox(("Log to file##pager_decoder_log_" + _this->name).c_str(),
                            &_this->logToFile))
        {
            _this->saveSettings();
        }
        if (_this->logToFile) {
            if (_this->logFolderSelect.render("##pager_decoder_logfolder_" + _this->name)) {
                _this->saveSettings();
            }
            if (_this->logFolderSelect.pathIsValid()) {
                ImGui::TextDisabled("File: pager_log.tsv");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "Invalid folder");
            }
        }

        if (!_this->enabled) { style::endDisabled(); }

        if (_this->showMessagesWindow) { _this->drawMessagesWindow(); }
    }

    void drawMessagesWindow() {
        std::string title = "Pager Messages (" + name + ")###pager_msg_" + name;
        ImGui::SetNextWindowSize(ImVec2(820, 400), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(title.c_str(), &showMessagesWindow)) {
            ImGui::End();
            return;
        }

        // Stop the waterfall from reacting to clicks/drags inside our window
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows
                                   | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
            || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        {
            gui::mainWindow.lockWaterfallControls = true;
        }

        // Toolbar
        if (ImGui::Button("Clear##pager_msg_clear")) {
            std::lock_guard<std::mutex> lck(messagesMtx);
            messages.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button("Save as TSV##pager_msg_save")) { saveAllMessagesToFile(); }
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll##pager_msg_autoscroll", &autoScroll);
        ImGui::SameLine();
        if (ImGui::Checkbox("Hide errors##pager_msg_hideerr", &hideErrorMessages)) { saveSettings(); }
        ImGui::SameLine();
        if (ImGui::Checkbox("Hide tone-only##pager_msg_hidetone", &hideToneOnly)) { saveSettings(); }

        const ImGuiTableFlags tableFlags =
            ImGuiTableFlags_Borders     |
            ImGuiTableFlags_RowBg       |
            ImGuiTableFlags_Resizable   |
            ImGuiTableFlags_ScrollY     |
            ImGuiTableFlags_SizingStretchProp;

        if (ImGui::BeginTable(("##pager_msg_table_" + name).c_str(), 6, tableFlags)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Timestamp", ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableSetupColumn("Proto",     ImGuiTableColumnFlags_WidthFixed,  60.0f);
            ImGui::TableSetupColumn("Address",   ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Type",      ImGuiTableColumnFlags_WidthFixed,  80.0f);
            ImGui::TableSetupColumn("Info",      ImGuiTableColumnFlags_WidthFixed,  80.0f);
            ImGui::TableSetupColumn("Message",   ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            {
                std::lock_guard<std::mutex> lck(messagesMtx);
                for (const auto& m : messages) {
                    if (hideErrorMessages && m.hasErrors)  { continue; }
                    if (hideToneOnly      && m.isToneOnly) { continue; }

                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(formatTimestamp(m.timestamp).c_str());

                    ImGui::TableSetColumnIndex(1);
                    if (m.protocol == UnifiedMessage::PROTO_POCSAG) {
                        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "POCSAG");
                    } else {
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "FLEX");
                    }

                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%lld", (long long)m.address);

                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(m.typeName.c_str());

                    ImGui::TableSetColumnIndex(4);
                    if (m.hasErrors) {
                        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m.info.c_str());
                    } else if (!m.info.empty() && m.info[0] == '+') {
                        ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%s", m.info.c_str());
                    } else {
                        ImGui::TextUnformatted(m.info.c_str());
                    }

                    ImGui::TableSetColumnIndex(5);
                    ImGui::TextUnformatted(m.content.c_str());
                }
            }

            if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10.0f) {
                ImGui::SetScrollHereY(1.0f);
            }

            ImGui::EndTable();
        }

        ImGui::End();
    }

    void saveAllMessagesToFile() {
        std::time_t now = std::time(nullptr);
        char fnbuf[64];
        std::strftime(fnbuf, sizeof(fnbuf), "pager_%Y%m%d_%H%M%S.tsv",
                      std::localtime(&now));

        std::string dir;
        if (logFolderSelect.pathIsValid() && !logFolderSelect.path.empty()) {
            dir = logFolderSelect.expandString(logFolderSelect.path);
        } else {
            dir = (std::string)core::args["root"];
        }
        std::string outPath = dir + "/" + fnbuf;

        std::ofstream f(outPath);
        if (!f.is_open()) {
            flog::error("Pager: cannot open {} for writing", outPath);
            return;
        }
        f << "timestamp\tprotocol\taddress\ttype\tinfo\tmessage\n";
        std::lock_guard<std::mutex> lck(messagesMtx);
        int written = 0;
        for (const auto& m : messages) {
            if (hideErrorMessages && m.hasErrors)  { continue; }
            if (hideToneOnly      && m.isToneOnly) { continue; }
            f << formatTimestamp(m.timestamp)
              << '\t' << protoName(m.protocol)
              << '\t' << m.address
              << '\t' << m.typeName
              << '\t' << m.info
              << '\t' << m.content
              << '\n';
            written++;
        }
        flog::info("Pager: saved {} messages to {}", written, outPath);
    }

    // -----------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------
    std::string                       name;
    bool                              enabled = true;

    VFOManager::VFO*                  vfo = nullptr;
    std::unique_ptr<POCSAGDecoder>    pocsagDecoder;
    std::unique_ptr<FLEXDecoder>      flexDecoder;
    Decoder*                          activeDecoder = nullptr;

    // Protocol selector
    int                                       protoId = 0;
    OptionList<Protocol, Protocol>            protocols;

    // Snap interval
    int                                       snapId = 0;
    OptionList<int, int>                      snapIntervals;

    // Message buffer
    std::mutex                                messagesMtx;
    std::deque<UnifiedMessage>                messages;

    // UI / runtime options
    bool                              showMessagesWindow = false;
    bool                              autoScroll         = true;
    bool                              hideErrorMessages  = true;
    bool                              hideToneOnly       = true;
    bool                              logToFile          = false;
    FolderSelect                      logFolderSelect;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    std::string root = (std::string)core::args["root"];
    config.setPath(root + "/pager_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new PagerDecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (PagerDecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
