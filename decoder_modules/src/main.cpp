/*
 * HFDL (High Frequency Data Link) decoder module for SDR++ (dumphfdl front-end).
 *
 * HFDL is the aeronautical HF data link that carries ACARS/AOC traffic plus the
 * ATN applications (CPDLC, ADS-C) between aircraft and a worldwide network of
 * ground stations. This module channelises a single HFDL channel to a 12 kHz
 * complex baseband and pipes it, as interleaved 16-bit I/Q (CS16), to a
 * dumphfdl child process (https://github.com/szpajder/dumphfdl). dumphfdl's
 * decoded text output is read back, shown in a detachable, filterable log
 * window, and — for messages that carry an aircraft position — forwarded as one
 * JSON object per line over an optional TCP connection (e.g. to a map server).
 *
 * Signal geometry: HFDL is transmitted on the upper sideband; the suppressed
 * carrier sits HFDL_SSB_CARRIER_OFFSET_HZ (1440 Hz) below the channel centre,
 * so when the VFO is tuned to the assigned channel frequency the modulation
 * lands around +1440 Hz in the baseband we hand to dumphfdl. Passing
 * "--centerfreq F --<channel> F" (both equal to the assigned kHz) makes
 * dumphfdl mix that +1440 Hz down to DC exactly as it does for an off-air
 * recording, while still printing the true channel frequency in each header.
 *
 * dumphfdl must be installed and reachable (on PATH or via the configured
 * path). This front-end does not decode anything itself.
 */
#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <gui/tuner.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <gui/widgets/folder_select.h>
#include <utils/optionlist.h>
#include <dsp/sink/handler_sink.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <cerrno>
#endif

#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <vector>
#include <string>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>

#include "systable_data.h"
#include "tcp_sender.h"
#include "hfdl_parse.h"

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "hfdl_decoder",
    /* Description:     */ "HFDL decoder (dumphfdl front-end)",
    /* Author:          */ "SDR++ Community",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

ConfigManager config;

// dumphfdl symbol rate is 1800 Bd; a 12 kHz complex baseband comfortably holds
// one 2.6 kHz-wide HFDL channel centred around +1440 Hz and matches the
// well-known KiwiSDR feeding recipe (--sample-rate 12000). Keeping the VFO at
// this rate means dumphfdl resamples internally and we never have to.
#define HFDL_SAMPLE_RATE   12000.0
// Bandwidth wide enough to pass the upper-sideband modulation (~+140..+2740 Hz)
// while rejecting neighbouring channels; dumphfdl does the final 2.6 kHz
// channelisation itself.
#define HFDL_BANDWIDTH     5800.0
// Low read-buffer keeps stdin latency down (must be a multiple of 4 for CS16).
#define HFDL_READ_BUF_BYTES "4800"
#define MAX_LOG_BLOCKS     4000

class HFDLDecoderModule : public ModuleManager::Instance {
public:
    HFDLDecoderModule(std::string name) : folderSelect("%ROOT%/recordings") {
        this->name = name;

#ifndef _WIN32
        signal(SIGPIPE, SIG_IGN); // we handle EPIPE when the child dies
#endif

        // Build the channel list from the embedded HFDL system table.
        for (int i = 0; i < HFDL_FREQ_CNT; i++) {
            const char* gs = stationName(HFDL_FREQS[i].primaryStationId);
            char lbl[80];
            std::snprintf(lbl, sizeof(lbl), "%.1f kHz \u2014 %s", HFDL_FREQS[i].khz, gs);
            channels.define(lbl, HFDL_FREQS[i].khz);
        }

        // Write the embedded system table to disk so dumphfdl can substitute
        // ground-station names/frequencies in its output (overridable below).
        defaultSysTablePath = core::args["root"].s() + "/hfdl_systable.conf";
        writeDefaultSysTable();
        sysTablePath = defaultSysTablePath;

        // Restore config.
        config.acquire();
        if (config.conf[name].contains("dumphfdlPath")) { dumphfdlPath = config.conf[name]["dumphfdlPath"]; }
        if (config.conf[name].contains("sysTablePath")) { sysTablePath = config.conf[name]["sysTablePath"]; }
        if (config.conf[name].contains("showWindow"))   { showWindow   = config.conf[name]["showWindow"];   }
        if (config.conf[name].contains("autoScroll"))   { autoScroll   = config.conf[name]["autoScroll"];   }
        if (config.conf[name].contains("recording"))    { recording    = config.conf[name]["recording"];    }
        if (config.conf[name].contains("recordPath"))   { folderSelect.setPath(config.conf[name]["recordPath"]); }
        if (config.conf[name].contains("tcpHost"))      { tcpHost      = config.conf[name]["tcpHost"];      }
        if (config.conf[name].contains("tcpPort"))      { tcpPort      = config.conf[name]["tcpPort"];      }
        if (config.conf[name].contains("channelId")) {
            std::string cn = config.conf[name]["channelId"];
            if (channels.keyExists(cn)) { chanId = channels.keyId(cn); }
        }
        // TCP is intentionally NOT auto-started, even if it was persisted as
        // enabled: a network connection only opens on explicit user action.
        tcpEnabled = false;
        config.release();

        std::strncpy(pathBuf, dumphfdlPath.c_str(), sizeof(pathBuf) - 1);
        std::strncpy(sysBuf,  sysTablePath.c_str(), sizeof(sysBuf) - 1);
        std::strncpy(hostBuf, tcpHost.c_str(),      sizeof(hostBuf) - 1);

        // VFO + sink. REF_CENTER: the assigned channel frequency lands at DC,
        // putting the upper-sideband modulation at ~+1440 Hz.
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER,
                                            0, HFDL_BANDWIDTH, HFDL_SAMPLE_RATE,
                                            HFDL_BANDWIDTH, HFDL_BANDWIDTH, true);
        vfo->setSnapInterval(100);
        sink.init(vfo->output, _sinkHandler, this);
        sink.start();

        // Tune to the restored channel.
        tuner::tune(tuner::TUNER_MODE_NORMAL, name, channels.value(chanId) * 1000.0);

        startDecoder();
        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~HFDLDecoderModule() {
        gui::menu.removeEntry(name);
        stopDecoder();                       // blocking (safe: tearing down)
        if (lifeThread.joinable()) { lifeThread.join(); }
        tcp.stop();
        if (enabled) {
            sink.stop();
            sigpath::vfoManager.deleteVFO(vfo);
        }
        closeLog();
        sigpath::sinkManager.unregisterStream(name);
    }

    void postInit() {}

    void enable() {
        double bw = gui::waterfall.getBandwidth();
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER,
                                            std::clamp<double>(0, -bw / 2.0, bw / 2.0),
                                            HFDL_BANDWIDTH, HFDL_SAMPLE_RATE,
                                            HFDL_BANDWIDTH, HFDL_BANDWIDTH, true);
        vfo->setSnapInterval(100);
        sink.setInput(vfo->output);
        sink.start();
        tuner::tune(tuner::TUNER_MODE_NORMAL, name, channels.value(chanId) * 1000.0);
        startDecoder();
        enabled = true;
    }

    void disable() {
        childStdin.store(-1);    // sink handler becomes a no-op immediately
        sink.stop();
        sigpath::vfoManager.deleteVFO(vfo);
        stopDecoderAsync();
        enabled = false;
    }

    bool isEnabled() { return enabled; }

private:
    static const char* stationName(int id) {
        for (int i = 0; i < HFDL_STATION_CNT; i++) {
            if (HFDL_STATIONS[i].id == id) { return HFDL_STATIONS[i].name; }
        }
        return "Unknown GS";
    }

    void writeDefaultSysTable() {
        FILE* f = fopen(defaultSysTablePath.c_str(), "w");
        if (!f) { return; }
        fputs(HFDL_SYSTABLE_CONF, f);
        fclose(f);
    }

    // ---------- DSP path: float complex -> int16 I/Q -> child stdin ----------
    static void _sinkHandler(dsp::complex_t* data, int count, void* ctx) {
        HFDLDecoderModule* _this = (HFDLDecoderModule*)ctx;
        int fd = _this->childStdin.load();
        if (fd < 0) { return; }

        _this->iqBuf.resize((size_t)count * 2);
        for (int i = 0; i < count; i++) {
            float re = data[i].re * 32768.0f;
            float im = data[i].im * 32768.0f;
            if (re >  32767.0f) re =  32767.0f; if (re < -32768.0f) re = -32768.0f;
            if (im >  32767.0f) im =  32767.0f; if (im < -32768.0f) im = -32768.0f;
            _this->iqBuf[2*i]   = (int16_t)lrintf(re);
            _this->iqBuf[2*i+1] = (int16_t)lrintf(im);
        }
#ifndef _WIN32
        const char* p = (const char*)_this->iqBuf.data();
        size_t total = _this->iqBuf.size() * sizeof(int16_t);
        size_t off = 0;
        while (off < total) {
            ssize_t w = write(fd, p + off, total - off);
            if (w > 0) { off += (size_t)w; continue; }
            if (w < 0 && (errno == EINTR)) { continue; }
            break; // EPIPE (child died) or EAGAIN
        }
#endif
    }

    // ---------- Child process lifecycle (never blocks the GUI thread) ----------
    void startDecoder()    { schedule([this]{ doStart(); }); }
    void stopDecoderAsync(){ schedule([this]{ doStop();  }); }
    void restartDecoder()  { schedule([this]{ doStop(); doStart(); }); }

    void schedule(std::function<void()> fn) {
#ifndef _WIN32
        if (lifeThread.joinable()) { lifeThread.join(); }
        lifeBusy.store(true);
        lifeThread = std::thread([this, fn]{
            std::lock_guard<std::mutex> lck(lifeMtx);
            fn();
            lifeBusy.store(false);
        });
#endif
    }

    void stopDecoder() {
#ifndef _WIN32
        if (lifeThread.joinable()) { lifeThread.join(); }
        std::lock_guard<std::mutex> lck(lifeMtx);
        doStop();
#endif
    }

    void doStart() {
#ifndef _WIN32
        if (running.load()) { return; }
        status = "starting...";

        int inPipe[2], outPipe[2];
        if (pipe(inPipe) != 0 || pipe(outPipe) != 0) { status = "pipe() failed"; return; }

        double fkhz = channels.value(chanId);
        char freqStr[32];
        std::snprintf(freqStr, sizeof(freqStr), "%.1f", fkhz);
        std::string path = pathBuf;
        if (path.empty()) { path = "dumphfdl"; }
        std::string sysPath = sysBuf;

        pid_t pid = fork();
        if (pid < 0) { status = "fork() failed"; close(inPipe[0]); close(inPipe[1]); close(outPipe[0]); close(outPipe[1]); return; }

        if (pid == 0) {
            // ---- child ----
            dup2(inPipe[0], STDIN_FILENO);
            dup2(outPipe[1], STDOUT_FILENO);
            close(inPipe[0]); close(inPipe[1]);
            close(outPipe[0]); close(outPipe[1]);
            // Build argv. centerfreq == channel == assigned kHz makes dumphfdl
            // mix the +1440 Hz upper sideband down to DC (as for a real file)
            // and print the true frequency in each message header.
            std::vector<std::string> args = {
                path, "--iq-file", "-",
                "--sample-format", "CS16",
                "--sample-rate", "12000",
                "--read-buffer-size", HFDL_READ_BUF_BYTES,
                "--utc",
                "--centerfreq", freqStr,
                freqStr,
                "--output", "decoded:text:file:path=-"
            };
            if (!sysPath.empty()) { args.push_back("--system-table"); args.push_back(sysPath); }
            std::vector<char*> argv;
            for (auto& a : args) { argv.push_back(const_cast<char*>(a.c_str())); }
            argv.push_back(nullptr);
            execvp(path.c_str(), argv.data());
            _exit(127); // execvp failed (e.g. dumphfdl not found)
        }

        // ---- parent ----
        close(inPipe[0]);
        close(outPipe[1]);
        childPid = pid;
        childStdout = outPipe[0];
        childStdin.store(inPipe[1]);
        running.store(true);
        curBlock.clear();
        readerThread = std::thread(&HFDLDecoderModule::readerLoop, this);
        status = "running";
#else
        status = "front-end mode is Linux-only";
#endif
    }

    void doStop() {
#ifndef _WIN32
        if (!running.load() && childPid <= 0) { return; }
        running.store(false);

        int sin = childStdin.exchange(-1);
        if (sin >= 0) { close(sin); }      // EOF to dumphfdl
        if (childPid > 0) { kill(childPid, SIGTERM); }
        if (readerThread.joinable()) { readerThread.join(); }
        if (childStdout >= 0) { close(childStdout); childStdout = -1; }
        if (childPid > 0) { int st; waitpid(childPid, &st, 0); childPid = -1; }
        status = "stopped";
#endif
    }

#ifndef _WIN32
    void readerLoop() {
        char buf[4096];
        std::string acc;
        while (running.load()) {
            ssize_t n = read(childStdout, buf, sizeof(buf));
            if (n > 0) {
                acc.append(buf, (size_t)n);
                size_t nl;
                while ((nl = acc.find('\n')) != std::string::npos) {
                    std::string line = acc.substr(0, nl);
                    acc.erase(0, nl + 1);
                    // A header line begins a new message block:
                    //   [2021-09-30 21:28:46 UTC] [11384.0 kHz] [24.7 Hz] [300 bps] [S]
                    bool isHeader = hfdlparse::isHeaderLine(line);
                    if (isHeader && !curBlock.empty()) {
                        pushBlock(curBlock);
                        curBlock.clear();
                    }
                    if (!curBlock.empty()) curBlock += "\n";
                    curBlock += line;
                }
            } else if (n == 0) {
                break;
            } else {
                if (errno == EINTR) { continue; }
                break;
            }
        }
        if (!curBlock.empty()) { pushBlock(curBlock); curBlock.clear(); }

        if (childPid > 0) {
            int st = 0;
            if (waitpid(childPid, &st, WNOHANG) == childPid) {
                childPid = -1;
                if (WIFEXITED(st) && WEXITSTATUS(st) == 127) {
                    status = "dumphfdl not found (check the path / PATH)";
                } else if (!running.load()) {
                    status = "stopped";
                } else {
                    status = "dumphfdl exited unexpectedly";
                }
            }
        }
        running.store(false);
    }
#endif

    void pushBlock(const std::string& block) {
        int t = hfdlparse::classify(block);
        {
            std::lock_guard<std::mutex> lck(msgMtx);
            messages.push_back({ block, t });
            if (messages.size() > MAX_LOG_BLOCKS) { messages.pop_front(); }
            newData = true;
        }
        writeLog(block);
        if (tcpEnabled) { maybeEmitPosition(block); }
    }

    // ---------- TCP map output ----------
    void maybeEmitPosition(const std::string& block) {
        std::string line;
        if (hfdlparse::buildPositionJson(block, line)) { tcp.send(line); }
    }

    // ---------- file logging ----------
    void openLog() {
        if (logFile || !folderSelect.pathIsValid()) { return; }
        std::string p = folderSelect.path + "/hfdl_dumphfdl.log";
        logFile = fopen(p.c_str(), "a");
    }
    void closeLog() { if (logFile) { fclose(logFile); logFile = nullptr; } }
    void writeLog(const std::string& block) {
        std::lock_guard<std::mutex> lck(logMtx);
        if (!recording || !logFile) { return; }
        fwrite(block.data(), 1, block.size(), logFile);
        fputc('\n', logFile);
        fflush(logFile);
    }

    void saveConfig() {
        config.acquire();
        config.conf[name]["dumphfdlPath"] = dumphfdlPath;
        config.conf[name]["sysTablePath"] = sysTablePath;
        config.conf[name]["tcpHost"]      = tcpHost;
        config.conf[name]["tcpPort"]      = tcpPort;
        config.release(true);
    }

    // ---------- GUI ----------
    static void menuHandler(void* ctx) {
        HFDLDecoderModule* _this = (HFDLDecoderModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) { style::beginDisabled(); }

        ImGui::LeftLabel("Channel");
        ImGui::FillWidth();
        if (ImGui::Combo(("##hfdl_chan_" + _this->name).c_str(), &_this->chanId, _this->channels.txt)) {
            tuner::tune(tuner::TUNER_MODE_NORMAL, _this->name, _this->channels.value(_this->chanId) * 1000.0);
            config.acquire();
            config.conf[_this->name]["channelId"] = _this->channels.key(_this->chanId);
            config.release(true);
            _this->restartDecoder();  // dumphfdl freq is set via argv -> respawn
        }

        ImGui::LeftLabel("dumphfdl");
        ImGui::FillWidth();
        if (ImGui::InputText(("##hfdl_path_" + _this->name).c_str(), _this->pathBuf, sizeof(_this->pathBuf))) {
            _this->dumphfdlPath = _this->pathBuf;
            _this->saveConfig();
        }

        ImGui::LeftLabel("Sys table");
        ImGui::FillWidth();
        if (ImGui::InputText(("##hfdl_sys_" + _this->name).c_str(), _this->sysBuf, sizeof(_this->sysBuf))) {
            _this->sysTablePath = _this->sysBuf;
            _this->saveConfig();
        }

        ImGui::Text("Status: %s", _this->status.c_str());
        if (_this->running.load()) {
            if (ImGui::Button(("Restart##hfdl_re_" + _this->name).c_str(), ImVec2(menuWidth, 0))) {
                _this->restartDecoder();
            }
        } else {
            if (ImGui::Button(("Start##hfdl_st_" + _this->name).c_str(), ImVec2(menuWidth, 0))) {
                _this->startDecoder();
            }
        }

        if (ImGui::Checkbox(("Log to file##hfdl_rec_" + _this->name).c_str(), &_this->recording)) {
            if (_this->recording) { _this->openLog(); } else { _this->closeLog(); }
            config.acquire();
            config.conf[_this->name]["recording"] = _this->recording;
            config.release(true);
        }
        if (_this->folderSelect.render("##hfdl_dir_" + _this->name)) {
            if (_this->folderSelect.pathIsValid()) {
                config.acquire();
                config.conf[_this->name]["recordPath"] = _this->folderSelect.path;
                config.release(true);
                if (_this->recording) { _this->closeLog(); _this->openLog(); }
            }
        }

        ImGui::Separator();

        // --- TCP map output ---
        ImGui::Text("TCP map output");
        ImGui::LeftLabel("Host");
        ImGui::FillWidth();
        if (ImGui::InputText(("##hfdl_host_" + _this->name).c_str(), _this->hostBuf, sizeof(_this->hostBuf))) {
            _this->tcpHost = _this->hostBuf;
            _this->saveConfig();
        }
        ImGui::LeftLabel("Port");
        ImGui::FillWidth();
        if (ImGui::InputInt(("##hfdl_port_" + _this->name).c_str(), &_this->tcpPort)) {
            if (_this->tcpPort < 1) _this->tcpPort = 1;
            if (_this->tcpPort > 65535) _this->tcpPort = 65535;
            _this->saveConfig();
        }
        if (ImGui::Checkbox(("Enable TCP##hfdl_tcp_en_" + _this->name).c_str(), &_this->tcpEnabled)) {
            if (_this->tcpEnabled) { _this->tcp.start(_this->tcpHost, _this->tcpPort); }
            else { _this->tcp.stop(); }
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

        if (ImGui::Button(("Show Messages##hfdl_show_" + _this->name).c_str(), ImVec2(menuWidth, 0))) {
            _this->showWindow = true;
            config.acquire();
            config.conf[_this->name]["showWindow"] = true;
            config.release(true);
        }

        if (!_this->enabled) { style::endDisabled(); }

        if (_this->showWindow) { _this->drawWindow(); }
    }

    void drawWindow() {
        ImGui::SetNextWindowSize(ImVec2(860, 480), ImGuiCond_FirstUseEver);
        std::string title = "HFDL Messages##" + name;
        bool open = ImGui::Begin(title.c_str(), &showWindow);

        ImGuiHoveredFlags hf = ImGuiHoveredFlags_RootAndChildWindows |
                               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem;
        if (ImGui::IsWindowHovered(hf) ||
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            gui::mainWindow.lockWaterfallControls = true;
        }
        if (!open) { ImGui::End(); return; }

        if (ImGui::Button(("Clear##hfdl_clr_" + name).c_str())) {
            std::lock_guard<std::mutex> lck(msgMtx);
            messages.clear();
        }
        ImGui::SameLine();
        ImGui::Checkbox(("Auto-scroll##hfdl_as_" + name).c_str(), &autoScroll);
        ImGui::SameLine();

        size_t counts[T_COUNT] = {0};
        size_t total = 0, shown = 0;
        {
            std::lock_guard<std::mutex> lck(msgMtx);
            total = messages.size();
            for (auto& m : messages) {
                int t = (m.type >= 0 && m.type < T_COUNT) ? m.type : T_OTHER;
                counts[t]++;
                if (typeFilter[t]) shown++;
            }
        }
        ImGui::Text("  %zu shown / %zu total", shown, total);

        ImGui::TextUnformatted("Filter:");
        ImGui::SameLine();
        if (ImGui::SmallButton(("All##hfdl_fall_" + name).c_str())) {
            for (int i = 0; i < T_COUNT; i++) typeFilter[i] = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(("None##hfdl_fnone_" + name).c_str())) {
            for (int i = 0; i < T_COUNT; i++) typeFilter[i] = false;
        }
        ImGui::SameLine();
        for (int i = 0; i < T_COUNT; i++) {
            char lbl[48];
            std::snprintf(lbl, sizeof(lbl), "%s (%zu)##hfdl_f%d_%s",
                          typeName(i), counts[i], i, name.c_str());
            ImGui::Checkbox(lbl, &typeFilter[i]);
            if (i != T_COUNT - 1) ImGui::SameLine();
        }
        ImGui::Separator();

        ImGui::BeginChild(("hfdl_log_" + name).c_str(), ImVec2(0, 0), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        {
            std::lock_guard<std::mutex> lck(msgMtx);
            for (auto& m : messages) {
                int t = (m.type >= 0 && m.type < T_COUNT) ? m.type : T_OTHER;
                if (!typeFilter[t]) continue;
                ImGui::TextUnformatted(m.text.c_str());
                ImGui::Separator();
            }
        }
        if (autoScroll && newData) {
            ImGui::SetScrollHereY(1.0f);
            newData = false;
        }
        ImGui::EndChild();

        ImGui::End();

        static bool lastShow = true;
        if (lastShow && !showWindow) {
            config.acquire();
            config.conf[name]["showWindow"] = false;
            config.release(true);
        }
        lastShow = showWindow;
    }

    std::string name;
    bool enabled = true;

    VFOManager::VFO* vfo = nullptr;
    dsp::sink::Handler<dsp::complex_t> sink;
    std::vector<int16_t> iqBuf;

    // child process
    std::atomic<int> childStdin{-1};
    int childStdout = -1;
    pid_t childPid = -1;
    std::thread readerThread;
    std::atomic<bool> running{false};
    std::string status = "idle";
    std::string curBlock;

    std::thread lifeThread;
    std::mutex lifeMtx;
    std::atomic<bool> lifeBusy{false};

    OptionList<std::string, double> channels;
    int chanId = 0;

    std::string dumphfdlPath = "dumphfdl";
    char pathBuf[512] = {0};
    std::string defaultSysTablePath;
    std::string sysTablePath;
    char sysBuf[512] = {0};

    // message log + filters (types defined in hfdl_parse.h). Mirror the two
    // enumerators we use as constants so they are valid in array bounds and in
    // member-function bodies regardless of textual order.
    static constexpr int T_OTHER = hfdlparse::T_OTHER;
    static constexpr int T_COUNT = hfdlparse::T_COUNT;
    static const char* typeName(int t) { return hfdlparse::typeName(t); }
    struct Msg { std::string text; int type; };
    std::mutex msgMtx;
    std::deque<Msg> messages;
    bool newData = false;
    bool typeFilter[T_COUNT] = { true,true,true,true,true,true,true };

    bool showWindow = false;
    bool autoScroll = true;

    FolderSelect folderSelect;
    bool recording = false;
    std::mutex logMtx;
    FILE* logFile = nullptr;

    // TCP map output
    TCPSender tcp;
    std::string tcpHost = "127.0.0.1";
    int tcpPort = 10100;
    bool tcpEnabled = false;
    char hostBuf[256] = {0};
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/hfdl_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new HFDLDecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (HFDLDecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
