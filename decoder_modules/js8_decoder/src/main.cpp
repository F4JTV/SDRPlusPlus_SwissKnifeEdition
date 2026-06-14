/*
 * JS8 decoder module for SDR++.
 *
 * Audio chain:
 *   VFO (complex baseband) -> dsp::demod::SSB<float> (USB) -> 12 kHz real audio
 *   -> UTC-slot-aligned buffer -> worker thread -> js8::decodeNormal().
 *
 * Decoded traffic is shown in a *detached, non-modal* window (a plain
 * ImGui::Begin()/End() pair, never a modal popup) so dragging it neither
 * darkens the SDR++ GUI nor moves the VFO. The VFO snap interval is set
 * explicitly so the default coarse grid does not pull the tuning.
 *
 * Each decode can also be streamed as one JSON line to a TCP endpoint
 * (default 127.0.0.1:10100) for the "SDR Map" backend. The TCP worker is
 * never started in the constructor and the enabled state is never persisted,
 * so TCP is always off at startup; only the host and port are remembered.
 *
 * Only the "Normal" submode (15 s, 6.25 Hz tone spacing, ORIGINAL Costas) is
 * wired end-to-end. The JS8 protocol was created by Jordan Sherer (KN4CRD);
 * the decode/encode core is ported from JS8Call (GPL-3.0).
 */

#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <signal_path/vfo_manager.h>
#include <gui/widgets/waterfall.h>
#include <config.h>
#include <core.h>
#include <utils/flog.h>

#include <dsp/demod/ssb.h>
#include <dsp/sink/handler_sink.h>

#include "js8_core.h"
#include "js8_varicode.h"
#include "tcp_sender.h"

#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <filesystem>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "js8_decoder",
    /* Description:     */ "JS8 decoder",
    /* Author:          */ "SDR++ Community",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

ConfigManager config;

// ft8_lib-style: the decoders expect 12 kHz USB audio.
static const double AUDIO_SR = 12000.0;

// One row shown in the results table.
struct DecodeRow {
    std::string time;   // UTC HH:MM:SS slot label
    std::string snr;    // dB
    std::string dt;     // seconds
    std::string freq;   // "1500 Hz"
    std::string type;   // frame type label
    std::string msg;    // interpreted message
};

// A captured audio slot waiting to be decoded on the worker thread.
struct AudioSlot {
    double startEpoch;          // UTC epoch (seconds) of the slot start
    double dialFreqMHz;         // absolute dial frequency at capture time
    std::vector<float> audio;   // 12 kHz mono USB audio
};

static const char* frameTypeName(int i3) {
    switch (i3) {
        case js8::FrameHeartbeat:        return "HB";
        case js8::FrameCompound:         return "COMPOUND";
        case js8::FrameCompoundDirected: return "COMP-DIR";
        case js8::FrameDirected:         return "DIRECTED";
        default:                         return "DATA";
    }
}

class JS8DecoderModule : public ModuleManager::Instance {
public:
    JS8DecoderModule(std::string name) {
        this->name = name;

        workdir = core::args["root"].s() + "/js8_decoder_" + name;
        try { std::filesystem::create_directories(workdir); } catch (...) {}

        loadSettings();

        double bw = bandwidthForMode();

        // Create the VFO and wire the DSP chain ONCE. DSP blocks are init'd a
        // single time here; enable()/disable() only start/stop the chain and
        // re-attach the VFO (re-init'ing an already-init'd block double-
        // registers its streams and crashes SDR++ on the second enable).
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_LOWER,
                                            0, bw, AUDIO_SR, 200, AUDIO_SR, false);
        vfo->setSnapInterval(snapIntervals[snapId]);

        // USB demod to real 12 kHz audio. AGC attack/decay are in 1/samples,
        // exactly like the stock radio module; raw values would make the AGC
        // far too fast and crush the signal dynamics.
        ssb.init(vfo->output, dsp::demod::SSB<float>::Mode::USB, bw, AUDIO_SR,
                 50.0 / AUDIO_SR, 5.0 / AUDIO_SR);
        sink.init(&ssb.out, audioHandler, this);

        running = true;
        worker = std::thread(&JS8DecoderModule::workerLoop, this);

        ssb.start();
        sink.start();

        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~JS8DecoderModule() {
        gui::menu.removeEntry(name);

        sink.stop();
        ssb.stop();

        running = false;
        cv.notify_all();
        if (worker.joinable()) { worker.join(); }

        tcp.stop();

        if (vfo) {
            sigpath::vfoManager.deleteVFO(vfo);
            vfo = NULL;
        }
    }

    void postInit() {}

    void enable() {
        double bw = bandwidthForMode();
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_LOWER,
                                            0, bw, AUDIO_SR, 200, AUDIO_SR, false);
        vfo->setSnapInterval(snapIntervals[snapId]);
        ssb.setInput(vfo->output);

        {
            std::lock_guard<std::mutex> lck(capMtx);
            cap.clear();
            capInit = false;
            firstPartial = true;
        }

        ssb.start();
        sink.start();
        enabled = true;
    }

    void disable() {
        sink.stop();
        ssb.stop();

        {
            std::lock_guard<std::mutex> lck(qMtx);
            queue.clear();
        }

        if (vfo) {
            sigpath::vfoManager.deleteVFO(vfo);
            vfo = NULL;
        }
        enabled = false;
    }

    bool isEnabled() { return enabled; }

private:
    // ---------------------------------------------------------------------
    double slotLength() const { return 15.0; }  // JS8 Normal submode
    double bandwidthForMode() const { return 3000.0; }  // 3 kHz USB window
    size_t minSamples() const {
        return (size_t)(slotLength() * AUDIO_SR * 0.90);
    }

    static double nowEpoch() {
        using namespace std::chrono;
        return duration_cast<duration<double>>(
                   system_clock::now().time_since_epoch()).count();
    }

    double currentDialMHz() const {
        double f = gui::waterfall.getCenterFrequency();
        if (vfo) { f += vfo->getOffset(); }
        return f / 1e6;
    }

    // ---------------------------------------------------------------------
    // Audio handler (DSP thread). Slices the stream into UTC-aligned 15 s
    // slots and hands full slots to the worker.
    // ---------------------------------------------------------------------
    static void audioHandler(float* data, int count, void* ctx) {
        JS8DecoderModule* _this = (JS8DecoderModule*)ctx;
        double slotLen = _this->slotLength();
        double now = nowEpoch();
        long long idx = (long long)std::floor(now / slotLen);

        std::lock_guard<std::mutex> lck(_this->capMtx);

        if (!_this->capInit) {
            _this->curIdx = idx;
            _this->capInit = true;
            _this->firstPartial = true;
            _this->cap.clear();
        }

        if (idx != _this->curIdx) {
            if (!_this->firstPartial && _this->cap.size() >= _this->minSamples()) {
                AudioSlot s;
                s.startEpoch = (double)_this->curIdx * slotLen;
                s.dialFreqMHz = _this->currentDialMHz();
                s.audio.swap(_this->cap);
                {
                    std::lock_guard<std::mutex> q(_this->qMtx);
                    _this->queue.push_back(std::move(s));
                }
                _this->cv.notify_one();
            }
            _this->cap.clear();
            _this->curIdx = idx;
            _this->firstPartial = false;
        }

        _this->cap.insert(_this->cap.end(), data, data + count);
    }

    // ---------------------------------------------------------------------
    // Worker thread: pops complete slots and runs the JS8 decoder.
    // ---------------------------------------------------------------------
    void workerLoop() {
        while (running) {
            AudioSlot slot;
            {
                std::unique_lock<std::mutex> lck(qMtx);
                cv.wait(lck, [this] { return !queue.empty() || !running; });
                if (!running) { break; }
                slot = std::move(queue.front());
                queue.pop_front();
            }
            decodeSlot(slot);
        }
    }

    void decodeSlot(const AudioSlot& slot) {
        std::time_t t = (std::time_t)slot.startEpoch;
        std::tm tmv;
#ifdef _WIN32
        gmtime_s(&tmv, &t);
#else
        gmtime_r(&t, &tmv);
#endif
        char hhmmss[16], date[16];
        std::strftime(hhmmss, sizeof(hhmmss), "%H:%M:%S", &tmv);
        std::strftime(date, sizeof(date), "%Y-%m-%d", &tmv);

        auto decodes = js8::decodeNormal(slot.audio.data(),
                                         slot.audio.size(),
                                         200.0f, 2800.0f);
        for (const auto& d : decodes) {
            std::string msg = js8::interpretMessage(d.token, d.i3);

            DecodeRow row;
            row.time = hhmmss;
            char b[48];
            snprintf(b, sizeof(b), "%.0f", d.snr);  row.snr = b;
            snprintf(b, sizeof(b), "%+.1f", d.dt);  row.dt = b;
            snprintf(b, sizeof(b), "%.0f Hz", d.f0); row.freq = b;
            row.type = frameTypeName(d.i3);
            row.msg = msg;
            addRow(row);

            if (tcpEnabled) { sendTcp(date, hhmmss, d, msg); }
        }
    }

    void sendTcp(const char* date, const char* time, const js8::Decode& d,
                 const std::string& msg) {
        // One JSON object per line: SDR Map schema.
        char info[256];
        snprintf(info, sizeof(info),
                 "freq=%.0f;dt=%.1f;snr=%.0f;i3=%d;type=%s;token=%s",
                 d.f0, d.dt, d.snr, d.i3, frameTypeName(d.i3),
                 d.token.c_str());

        json j;
        j["name"]  = msg;
        j["date"]  = date;
        j["time"]  = time;
        j["lat"]   = nullptr;
        j["lon"]   = nullptr;
        j["type"]  = "JS8";
        j["speed"] = nullptr;
        j["info"]  = std::string(info);
        tcp.send(j.dump());
    }

    void addRow(const DecodeRow& row) {
        std::lock_guard<std::mutex> lck(rowsMtx);
        rows.push_back(row);
        while (rows.size() > MAX_ROWS) { rows.pop_front(); }
        scrollToBottom = true;
    }

    // ---------------------------------------------------------------------
    // Settings. The TCP enabled flag is intentionally NOT persisted (TCP is
    // always off at startup); the host and port ARE persisted.
    // ---------------------------------------------------------------------
    void loadSettings() {
        config.acquire();
        if (!config.conf.contains(name)) { config.conf[name] = json::object(); }
        json& c = config.conf[name];
        if (c.contains("snapId"))     { snapId = c["snapId"].get<int>(); }
        if (c.contains("showWindow")) { showWindow = c["showWindow"].get<bool>(); }
        if (c.contains("tcpHost"))    { tcpHost = c["tcpHost"].get<std::string>(); }
        if (c.contains("tcpPort"))    { tcpPort = c["tcpPort"].get<int>(); }
        config.release();

        if (snapId < 0 || snapId >= (int)(sizeof(snapIntervals)/sizeof(snapIntervals[0]))) {
            snapId = 0;
        }
        std::strncpy(tcpHostBuf, tcpHost.c_str(), sizeof(tcpHostBuf) - 1);
    }

    void saveSettings() {
        config.acquire();
        config.conf[name]["snapId"]     = snapId;
        config.conf[name]["showWindow"] = showWindow;
        config.conf[name]["tcpHost"]    = tcpHost;
        config.conf[name]["tcpPort"]    = tcpPort;
        config.release(true);
    }

    // ---------------------------------------------------------------------
    // GUI.
    // ---------------------------------------------------------------------
    static void menuHandler(void* ctx) {
        JS8DecoderModule* _this = (JS8DecoderModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) { style::beginDisabled(); }

        ImGui::LeftLabel("Submode");
        ImGui::FillWidth();
        const char* modeTxt = "Normal\0";
        int m = 0;
        ImGui::Combo(CONCAT("##js8dec_mode_", _this->name), &m, modeTxt);

        ImGui::LeftLabel("Snap");
        ImGui::FillWidth();
        const char* snapTxt = "1 Hz\0" "10 Hz\0" "100 Hz\0" "1 kHz\0" "2.5 kHz\0";
        if (ImGui::Combo(CONCAT("##js8dec_snap_", _this->name), &_this->snapId, snapTxt)) {
            if (_this->vfo) { _this->vfo->setSnapInterval(_this->snapIntervals[_this->snapId]); }
            _this->saveSettings();
        }

        ImGui::Separator();

        // ---- TCP output -------------------------------------------------
        if (ImGui::Checkbox(CONCAT("Enable TCP##js8dec_tcp_", _this->name),
                            &_this->tcpEnabled)) {
            if (_this->tcpEnabled) {
                _this->tcp.start(_this->tcpHost, _this->tcpPort);
            } else {
                _this->tcp.stop();
            }
        }
        ImGui::SameLine();
        if (!_this->tcpEnabled) {
            ImGui::TextDisabled("disabled");
        } else if (_this->tcp.isConnected()) {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "connected");
        } else {
            ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "enabled");
        }

        ImGui::LeftLabel("Host");
        ImGui::FillWidth();
        if (ImGui::InputText(CONCAT("##js8dec_host_", _this->name),
                             _this->tcpHostBuf, sizeof(_this->tcpHostBuf))) {
            _this->tcpHost = _this->tcpHostBuf;
            _this->saveSettings();
        }
        ImGui::LeftLabel("Port");
        ImGui::FillWidth();
        if (ImGui::InputInt(CONCAT("##js8dec_port_", _this->name),
                            &_this->tcpPort, 0, 0)) {
            if (_this->tcpPort < 1) { _this->tcpPort = 1; }
            if (_this->tcpPort > 65535) { _this->tcpPort = 65535; }
            _this->saveSettings();
        }

        ImGui::Separator();

        if (ImGui::Button(CONCAT("Show Decodes##js8dec_show_", _this->name),
                          ImVec2(menuWidth, 0))) {
            _this->showWindow = true;
            _this->saveSettings();
        }

        if (!_this->enabled) { style::endDisabled(); }

        ImGui::TextDisabled("Sync the PC clock (NTP) for decoding.");

        if (_this->showWindow) { _this->drawDecodesWindow(); }
    }

    // Detached window — same pattern as the stock POCSAG/frequency_manager
    // modules: a stable "###" ID, no window flags, and lockWaterfallControls
    // asserted whenever the user interacts with it (SDR++ resets the flag each
    // frame, so we only ever assert it).
    void drawDecodesWindow() {
        std::string title = "JS8 Decodes (" + name + ")###js8dec_win_" + name;
        ImGui::SetNextWindowSize(ImVec2(680, 360), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(title.c_str(), &showWindow)) {
            ImGui::End();
            return;
        }

        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                                   ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
            (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
             ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
            gui::mainWindow.lockWaterfallControls = true;
        }

        if (ImGui::Button(CONCAT("Clear##js8dec_clear_", name))) {
            std::lock_guard<std::mutex> lck(rowsMtx);
            rows.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button(CONCAT("Save TSV##js8dec_save_", name))) {
            saveTsvSnapshot();
        }
        ImGui::SameLine();
        ImGui::Checkbox(CONCAT("Auto-scroll##js8dec_auto_", name), &autoScroll);

        ImGui::Separator();

        ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
        if (ImGui::BeginTable(CONCAT("##js8dec_table_", name), 5, flags)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("UTC", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("dB",  ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableSetupColumn("DT",  ImGuiTableColumnFlags_WidthFixed, 45.0f);
            ImGui::TableSetupColumn("Hz",  ImGuiTableColumnFlags_WidthFixed, 65.0f);
            ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            {
                std::lock_guard<std::mutex> lck(rowsMtx);
                for (const auto& r : rows) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(r.time.c_str());
                    ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(r.snr.c_str());
                    ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(r.dt.c_str());
                    ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(r.freq.c_str());
                    ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(r.msg.c_str());
                }
            }

            if (autoScroll && scrollToBottom) {
                ImGui::SetScrollHereY(1.0f);
                scrollToBottom = false;
            }
            ImGui::EndTable();
        }

        ImGui::End();
    }

    void saveTsvSnapshot() {
        std::string path = workdir + "/decodes_snapshot.tsv";
        FILE* f = fopen(path.c_str(), "w");
        if (!f) { return; }
        fprintf(f, "UTC\tdB\tDT\tHz\tType\tMessage\n");
        std::lock_guard<std::mutex> lck(rowsMtx);
        for (const auto& r : rows) {
            fprintf(f, "%s\t%s\t%s\t%s\t%s\t%s\n",
                    r.time.c_str(), r.snr.c_str(), r.dt.c_str(),
                    r.freq.c_str(), r.type.c_str(), r.msg.c_str());
        }
        fclose(f);
        flog::info("JS8 decoder: saved snapshot to {0}", path);
    }

    // ---------------------------------------------------------------------
    std::string name;
    bool enabled = true;

    int snapId = 0;
    const double snapIntervals[5] = { 1.0, 10.0, 100.0, 1000.0, 2500.0 };
    bool showWindow = false;

    // TCP output (enabled flag NOT persisted; host/port persisted).
    TcpLineSender tcp;
    bool tcpEnabled = false;
    std::string tcpHost = "127.0.0.1";
    int tcpPort = 10100;
    char tcpHostBuf[256] = { 0 };

    // DSP.
    VFOManager::VFO* vfo = NULL;
    dsp::demod::SSB<float> ssb;
    dsp::sink::Handler<float> sink;

    std::string workdir;

    // Slot capture (DSP thread).
    std::mutex capMtx;
    std::vector<float> cap;
    long long curIdx = 0;
    bool capInit = false;
    bool firstPartial = true;

    // Decode queue + worker.
    std::thread worker;
    std::atomic<bool> running{ false };
    std::mutex qMtx;
    std::condition_variable cv;
    std::deque<AudioSlot> queue;

    // Results.
    static const size_t MAX_ROWS = 2000;
    std::mutex rowsMtx;
    std::deque<DecodeRow> rows;
    bool autoScroll = true;
    bool scrollToBottom = false;
};

// --------------------------------------------------------------------------
MOD_EXPORT void _INIT_() {
    json def = json::object();
    config.setPath(core::args["root"].s() + "/js8_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new JS8DecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (JS8DecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
}
