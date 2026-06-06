/*
 * VDL Mode 2 decoder module for SDR++ (dumpvdl2 front-end).
 *
 * This module channelises a VDL2 voice-channel-width slice of spectrum to
 * 105 kHz complex baseband and pipes it, as interleaved 16-bit I/Q, to a
 * dumpvdl2 child process (https://github.com/szpajder/dumpvdl2). dumpvdl2's
 * text output is read back and shown in a detachable log window, giving the
 * full VDL2 decode (ACARS, X.25/CLNP, CPDLC, ADS-C, ...) integrated into the
 * SDR++ UI.
 *
 * dumpvdl2 must be installed and reachable (on PATH or via the configured
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
#include <cstring>
#include <cmath>
#include <ctime>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "vdl2_decoder",
    /* Description:     */ "VDL Mode 2 decoder (dumpvdl2 front-end)",
    /* Author:          */ "SDR++ Community",
    /* Version:         */ 0, 3, 0,
    /* Max instances    */ -1
};

ConfigManager config;

// VFO output rate = SYMBOL_RATE(10500) * SPS(10) = dumpvdl2 base rate at
// --oversample 1. Feeding this rate means no resampling is needed.
#define VDL2_SAMPLE_RATE   105000.0
#define VDL2_BANDWIDTH     14000.0
#define MAX_LOG_BLOCKS     4000

class VDL2DecoderModule : public ModuleManager::Instance {
public:
    VDL2DecoderModule(std::string name) : folderSelect("%ROOT%/recordings") {
        this->name = name;

#ifndef _WIN32
        // Don't die if the child's stdin pipe breaks; we handle EPIPE.
        signal(SIGPIPE, SIG_IGN);
#endif

        // Standard VDL2 channels (Hz).
        channels.define("136.975 (CSC)", 136975000.0);
        channels.define("136.725",       136725000.0);
        channels.define("136.775",       136775000.0);
        channels.define("136.825",       136825000.0);
        channels.define("136.875",       136875000.0);
        channels.define("136.700",       136700000.0);
        channels.define("136.800",       136800000.0);
        channels.define("136.925",       136925000.0);

        // Restore config.
        config.acquire();
        if (config.conf[name].contains("dumpvdl2Path")) { dumpvdl2Path = config.conf[name]["dumpvdl2Path"]; }
        if (config.conf[name].contains("showWindow"))   { showWindow   = config.conf[name]["showWindow"];   }
        if (config.conf[name].contains("autoScroll"))   { autoScroll   = config.conf[name]["autoScroll"];   }
        if (config.conf[name].contains("recording"))    { recording    = config.conf[name]["recording"];    }
        if (config.conf[name].contains("recordPath"))   { folderSelect.setPath(config.conf[name]["recordPath"]); }
        if (config.conf[name].contains("channelId")) {
            std::string cn = config.conf[name]["channelId"];
            if (channels.keyExists(cn)) { chanId = channels.keyId(cn); }
        }
        config.release();
        std::strncpy(pathBuf, dumpvdl2Path.c_str(), sizeof(pathBuf) - 1);

        // VFO + sink.
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER,
                                            0, VDL2_BANDWIDTH, VDL2_SAMPLE_RATE,
                                            VDL2_BANDWIDTH, VDL2_BANDWIDTH, true);
        vfo->setSnapInterval(25000);
        sink.init(vfo->output, _sinkHandler, this);
        sink.start();

        startDecoder();
        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~VDL2DecoderModule() {
        gui::menu.removeEntry(name);
        stopDecoder();                       // blocking (safe: tearing down)
        if (lifeThread.joinable()) { lifeThread.join(); }
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
                                            VDL2_BANDWIDTH, VDL2_SAMPLE_RATE,
                                            VDL2_BANDWIDTH, VDL2_BANDWIDTH, true);
        vfo->setSnapInterval(25000);
        sink.setInput(vfo->output);
        sink.start();
        startDecoder();          // async; GUI does not block
        enabled = true;
    }

    void disable() {
        // Stop feeding samples first, then tear down the child in the
        // background so the GUI thread doesn't stall on the child exit.
        childStdin.store(-1);    // sink handler becomes a no-op immediately
        sink.stop();
        sigpath::vfoManager.deleteVFO(vfo);
        stopDecoderAsync();
        enabled = false;
    }

    bool isEnabled() { return enabled; }

private:
    // ---------- DSP path: float complex -> int16 I/Q -> child stdin ----------
    static void _sinkHandler(dsp::complex_t* data, int count, void* ctx) {
        VDL2DecoderModule* _this = (VDL2DecoderModule*)ctx;
        int fd = _this->childStdin.load();
        if (fd < 0) { return; }

        // Convert to interleaved S16_LE (dumpvdl2 divides by 32768 on input).
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
            // EPIPE (child died) or EAGAIN: stop pushing this block.
            break;
        }
#endif
    }

    // ---------- Child process lifecycle ----------
    // ---- Public lifecycle entry points: never block the GUI thread. ----
    void startDecoder()   { schedule([this]{ doStart(); }); }
    void stopDecoderAsync(){ schedule([this]{ doStop();  }); }
    void restartDecoder() { schedule([this]{ doStop(); doStart(); }); }

    // Run a lifecycle transition on a detached-ish background thread. Only one
    // runs at a time (joined before the next is launched); the GUI returns
    // immediately.
    void schedule(std::function<void()> fn) {
#ifndef _WIN32
        if (lifeThread.joinable()) { lifeThread.join(); } // previous transition done
        lifeBusy.store(true);
        lifeThread = std::thread([this, fn]{
            std::lock_guard<std::mutex> lck(lifeMtx);
            fn();
            lifeBusy.store(false);
        });
#endif
    }

    // Blocking stop, used only from the destructor (GUI is already tearing down).
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

        int inPipe[2], outPipe[2];   // inPipe: parent->child stdin; outPipe: child stdout->parent
        if (pipe(inPipe) != 0 || pipe(outPipe) != 0) { status = "pipe() failed"; return; }

        double f = channels.value(chanId);
        char freqStr[32];
        snprintf(freqStr, sizeof(freqStr), "%lld", (long long)f);
        std::string path = pathBuf;
        if (path.empty()) { path = "dumpvdl2"; }

        pid_t pid = fork();
        if (pid < 0) { status = "fork() failed"; close(inPipe[0]); close(inPipe[1]); close(outPipe[0]); close(outPipe[1]); return; }

        if (pid == 0) {
            // ---- child ----
            dup2(inPipe[0], STDIN_FILENO);
            dup2(outPipe[1], STDOUT_FILENO);
            close(inPipe[0]); close(inPipe[1]);
            close(outPipe[0]); close(outPipe[1]);
            // The VFO already centres the channel at DC, so freq == centerfreq
            // (mixer offset 0). --oversample 1 -> 105000 S/s, matching the VFO.
            execlp(path.c_str(), path.c_str(),
                   "--iq-file", "-",
                   "--sample-format", "S16_LE",
                   "--oversample", "1",
                   "--centerfreq", freqStr,
                   freqStr,
                   "--output", "decoded:text:file:path=-",
                   (char*)NULL);
            _exit(127); // execlp failed (e.g. dumpvdl2 not found)
        }

        // ---- parent ----
        close(inPipe[0]);   // parent writes to inPipe[1]
        close(outPipe[1]);  // parent reads from outPipe[0]
        childPid = pid;
        childStdout = outPipe[0];
        childStdin.store(inPipe[1]);
        running.store(true);
        readerThread = std::thread(&VDL2DecoderModule::readerLoop, this);
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
        if (sin >= 0) { close(sin); }      // closing stdin signals EOF to dumpvdl2
        if (childPid > 0) { kill(childPid, SIGTERM); }
        if (readerThread.joinable()) { readerThread.join(); }
        if (childStdout >= 0) { close(childStdout); childStdout = -1; }
        if (childPid > 0) {
            int st; waitpid(childPid, &st, 0); childPid = -1;
        }
        status = "stopped";
#endif
    }

#ifndef _WIN32
    void readerLoop() {
        std::string acc;        // accumulates the current message block
        char buf[4096];
        bool sawAny = false;
        while (running.load()) {
            ssize_t n = read(childStdout, buf, sizeof(buf));
            if (n > 0) {
                sawAny = true;
                acc.append(buf, (size_t)n);
                // Split into lines; a header line (starts with '[' and contains
                // "dBFS") begins a new message block.
                size_t nl;
                while ((nl = acc.find('\n')) != std::string::npos) {
                    std::string line = acc.substr(0, nl);
                    acc.erase(0, nl + 1);
                    bool isHeader = (!line.empty() && line[0] == '[' &&
                                     line.find("dBFS") != std::string::npos);
                    if (isHeader && !curBlock.empty()) {
                        pushBlock(curBlock);
                        curBlock.clear();
                    }
                    if (!curBlock.empty()) curBlock += "\n";
                    curBlock += line;
                }
            } else if (n == 0) {
                break;  // child closed stdout (exited)
            } else {
                if (errno == EINTR) { continue; }
                break;
            }
        }
        if (!curBlock.empty()) { pushBlock(curBlock); curBlock.clear(); }

        // Determine why the child ended.
        if (childPid > 0) {
            int st = 0;
            if (waitpid(childPid, &st, WNOHANG) == childPid) {
                childPid = -1;
                if (WIFEXITED(st) && WEXITSTATUS(st) == 127) {
                    status = "dumpvdl2 not found (check the path / PATH)";
                } else if (!running.load()) {
                    status = "stopped";
                } else {
                    status = "dumpvdl2 exited unexpectedly";
                }
            }
        }
        if (!sawAny && running.load()) { /* leave status as-is */ }
        running.store(false);
    }
#endif

    // Classify a decoded text block by the highest/most-specific layer present,
    // using the labels dumpvdl2 prints.
    static int classify(const std::string& b) {
        if (b.find("CPDLC") != std::string::npos)        return T_CPDLC;
        if (b.find("ADS-C") != std::string::npos ||
            b.find("ADS-v") != std::string::npos)        return T_ADSC;
        if (b.find("ACARS:") != std::string::npos)       return T_ACARS;
        // CLNP is carried inside X.25, so check the more specific layer first.
        if (b.find("CLNP") != std::string::npos)          return T_CLNP;
        if (b.find("X.25") != std::string::npos)          return T_X25;
        // AVLC supervisory/unnumbered control frames with no upper layer.
        if (b.find("AVLC type: S") != std::string::npos ||
            b.find("AVLC type: U") != std::string::npos)  return T_AVLC;
        return T_OTHER;
    }

    void pushBlock(const std::string& block) {
        int t = classify(block);
        {
            std::lock_guard<std::mutex> lck(msgMtx);
            messages.push_back({ block, t });
            if (messages.size() > MAX_LOG_BLOCKS) { messages.pop_front(); }
            newData = true;
        }
        writeLog(block);
    }

    // ---------- file logging ----------
    void openLog() {
        if (logFile || !folderSelect.pathIsValid()) { return; }
        std::string p = folderSelect.path + "/vdl2_dumpvdl2.log";
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

    // ---------- GUI ----------
    static void menuHandler(void* ctx) {
        VDL2DecoderModule* _this = (VDL2DecoderModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) { style::beginDisabled(); }

        ImGui::LeftLabel("Channel");
        ImGui::FillWidth();
        if (ImGui::Combo(("##vdl2_chan_" + _this->name).c_str(), &_this->chanId, _this->channels.txt)) {
            tuner::tune(tuner::TUNER_MODE_NORMAL, _this->name, _this->channels.value(_this->chanId));
            config.acquire();
            config.conf[_this->name]["channelId"] = _this->channels.key(_this->chanId);
            config.release(true);
            _this->restartDecoder();  // dumpvdl2 freq is set via argv -> respawn
        }

        ImGui::LeftLabel("dumpvdl2");
        ImGui::FillWidth();
        if (ImGui::InputText(("##vdl2_path_" + _this->name).c_str(), _this->pathBuf, sizeof(_this->pathBuf))) {
            _this->dumpvdl2Path = _this->pathBuf;
            config.acquire();
            config.conf[_this->name]["dumpvdl2Path"] = _this->dumpvdl2Path;
            config.release(true);
        }

        ImGui::Text("Status: %s", _this->status.c_str());
        if (_this->running.load()) {
            if (ImGui::Button(("Restart##vdl2_re_" + _this->name).c_str(), ImVec2(menuWidth, 0))) {
                _this->restartDecoder();
            }
        } else {
            if (ImGui::Button(("Start##vdl2_st_" + _this->name).c_str(), ImVec2(menuWidth, 0))) {
                _this->startDecoder();
            }
        }

        if (ImGui::Checkbox(("Log to file##vdl2_rec_" + _this->name).c_str(), &_this->recording)) {
            if (_this->recording) { _this->openLog(); } else { _this->closeLog(); }
            config.acquire();
            config.conf[_this->name]["recording"] = _this->recording;
            config.release(true);
        }
        if (_this->folderSelect.render("##vdl2_dir_" + _this->name)) {
            if (_this->folderSelect.pathIsValid()) {
                config.acquire();
                config.conf[_this->name]["recordPath"] = _this->folderSelect.path;
                config.release(true);
                if (_this->recording) { _this->closeLog(); _this->openLog(); }
            }
        }

        if (ImGui::Button(("Show Messages##vdl2_show_" + _this->name).c_str(), ImVec2(menuWidth, 0))) {
            _this->showWindow = true;
            config.acquire();
            config.conf[_this->name]["showWindow"] = true;
            config.release(true);
        }

        if (!_this->enabled) { style::endDisabled(); }

        if (_this->showWindow) { _this->drawWindow(); }
    }

    void drawWindow() {
        ImGui::SetNextWindowSize(ImVec2(820, 480), ImGuiCond_FirstUseEver);
        std::string title = "VDL2 Messages##" + name;
        bool open = ImGui::Begin(title.c_str(), &showWindow);

        // Keep the waterfall VFO from being retuned while interacting here.
        ImGuiHoveredFlags hf = ImGuiHoveredFlags_RootAndChildWindows |
                               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem;
        if (ImGui::IsWindowHovered(hf) ||
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            gui::mainWindow.lockWaterfallControls = true;
        }
        if (!open) { ImGui::End(); return; }

        if (ImGui::Button(("Clear##vdl2_clr_" + name).c_str())) {
            std::lock_guard<std::mutex> lck(msgMtx);
            messages.clear();
        }
        ImGui::SameLine();
        ImGui::Checkbox(("Auto-scroll##vdl2_as_" + name).c_str(), &autoScroll);
        ImGui::SameLine();

        // Per-type counts (for the filter labels and the total).
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

        // Filter row: one toggle per message type, with All / None shortcuts.
        ImGui::TextUnformatted("Filter:");
        ImGui::SameLine();
        if (ImGui::SmallButton(("All##vdl2_fall_" + name).c_str())) {
            for (int i = 0; i < T_COUNT; i++) typeFilter[i] = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(("None##vdl2_fnone_" + name).c_str())) {
            for (int i = 0; i < T_COUNT; i++) typeFilter[i] = false;
        }
        ImGui::SameLine();
        for (int i = 0; i < T_COUNT; i++) {
            char lbl[48];
            snprintf(lbl, sizeof(lbl), "%s (%zu)##vdl2_f%d_%s",
                     typeName(i), counts[i], i, name.c_str());
            ImGui::Checkbox(lbl, &typeFilter[i]);
            if (i != T_COUNT - 1) ImGui::SameLine();
        }
        ImGui::Separator();

        ImGui::BeginChild(("vdl2_log_" + name).c_str(), ImVec2(0, 0), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PushFont(NULL); // default font (monospace-ish handled by theme)
        {
            std::lock_guard<std::mutex> lck(msgMtx);
            for (auto& m : messages) {
                int t = (m.type >= 0 && m.type < T_COUNT) ? m.type : T_OTHER;
                if (!typeFilter[t]) continue;
                ImGui::TextUnformatted(m.text.c_str());
                ImGui::Separator();
            }
        }
        ImGui::PopFont();
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

    // Lifecycle transitions (start/stop/restart) run on this background thread
    // so the GUI thread never blocks on join()/waitpid(). lifeMtx serializes
    // them; lifeBusy reflects an in-flight transition for the UI.
    std::thread lifeThread;
    std::mutex lifeMtx;
    std::atomic<bool> lifeBusy{false};

    OptionList<std::string, double> channels;
    int chanId = 0;

    std::string dumpvdl2Path = "dumpvdl2";
    char pathBuf[512] = {0};

    // A decoded message block plus its detected type (for filtering).
    enum MsgType { T_ACARS=0, T_X25, T_CLNP, T_CPDLC, T_ADSC, T_AVLC, T_OTHER, T_COUNT };
    static const char* typeName(int t) {
        static const char* n[T_COUNT] = {"ACARS","X.25","CLNP","CPDLC","ADS-C","AVLC ctl","Other"};
        return (t>=0 && t<T_COUNT) ? n[t] : "Other";
    }
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
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/vdl2_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new VDL2DecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (VDL2DecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
