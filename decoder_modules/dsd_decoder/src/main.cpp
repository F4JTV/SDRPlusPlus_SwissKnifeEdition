#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <gui/main_window.h>
#include <gui/widgets/folder_select.h>
#include <signal_path/signal_path.h>
#include <signal_path/sink.h>
#include <module.h>
#include <utils/optionlist.h>
#include <utils/flog.h>
#include <utils/event.h>

#include <dsp/stream.h>
#include <dsp/demod/fm.h>
#include <dsp/sink/handler_sink.h>

#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <thread>
#include <memory>
#include <condition_variable>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <regex>
#include <ctime>
#include <cstdio>
#include <cstring>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#endif

#include "subprocess.h"
#include "tcp_sender.h"
#include "rigctl_internal.h"
#include "rigctl_client.h"
#include <gui/tuner.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "dsd_decoder",
    /* Description:     */ "Digital voice / data decoder (DMR, P25, NXDN, dPMR, YSF, ProVoice, EDACS, X2-TDMA, M17) via DSD-FME",
    /* Author:          */ "SDR++ Community",
    /* Version:         */ 0, 7, 3,
    /* Max instances    */ -1
};

ConfigManager config;

// Input audio contract: FM-discriminated, 48 kHz mono s16le, piped to dsd-fme's stdin.
static constexpr double DSD_INPUT_SR  = 48000.0;
// Output audio contract from dsd-fme over UDP: s16le, 8 kHz. Channel count
// depends on the protocol (typically mono; DMR Stereo interleaves slot1/slot2).
// We treat the stream as mono-equivalent: each s16 sample is upsampled 6x and
// duplicated to both stereo channels before writing to the SDR++ sink chain.
static constexpr int    DSD_OUTPUT_SR = 8000;
// Sample rate at which we publish the output audio stream to SDR++'s sink.
static constexpr float  SDRPP_OUT_SR  = 48000.0f;
static constexpr int    UPSAMPLE_RATIO = 6; // 8000 -> 48000

struct LrrpEntry {
    std::string time;
    std::string source;
    std::string lat;
    std::string lon;
    std::string speed;   // km/h, empty if unknown
    std::string dir;     // degrees x2 from frame, empty if unknown
    std::string raw;
};

struct CallEntry {
    std::time_t startTs        = 0;
    std::time_t lastActivityTs = 0;
    std::time_t endTs          = 0;   // valid once closed == true
    std::string protocol;             // "DMR", "P25P1", "NXDN48", ...
    std::string tg;                   // talkgroup
    std::string src;                  // source radio ID (a.k.a. RID)
    int         slot = 0;             // 0 = unknown / N/A, 1 or 2 for DMR
    std::string cc;                   // DMR color code (or generic CC)
    std::string nac;                  // P25 NAC
    bool        enc    = false;
    bool        closed = false;
};

static constexpr size_t CONSOLE_MAX_LINES = 5000;
static constexpr size_t LRRP_MAX_ROWS     = 2000;
static constexpr size_t CALLS_MAX_ROWS    = 5000;
static constexpr int    CALL_HANG_SEC     = 2;        // call considered ended after this gap
static constexpr size_t PCM_QUEUE_MAX     = 48000 * 5; // 5 s of input backlog

struct ConsoleLine {
    std::time_t ts;
    std::string text;
};

class DSDDecoderModule : public ModuleManager::Instance {
public:
    DSDDecoderModule(std::string n) {
        this->name = n;

        // ---- option lists ---------------------------------------------------
        modes.define("Auto (DMR / P25)",            "");
        modes.define("XDMA (P25p1/p2, YSF, DMRs)",  "-ft");
        modes.define("DMR Stereo",                  "-fs");
        modes.define("P25 Phase 1",                 "-f1");
        modes.define("P25 Phase 2",                 "-f2");
        modes.define("NXDN48",                      "-fi");
        modes.define("NXDN96",                      "-fn");
        modes.define("dPMR",                        "-fm");
        modes.define("YSF (System Fusion)",         "-fy");
        modes.define("ProVoice",                    "-fp");
        modes.define("EDACS Std/Net",               "-fh");
        modes.define("EDACS Std/Net (ESK)",         "-fH");
        modes.define("EDACS Extended Addr",         "-fe");
        modes.define("EDACS Extended Addr (ESK)",   "-fE");
        modes.define("X2-TDMA",                     "-fx");
        modes.define("M17 Stream",                  "-fz");

        // Snap-interval combobox values per the user spec.
        snapIntervals.define(100,   "100 Hz",  100.0);
        snapIntervals.define(1000,  "1 kHz",   1000.0);
        snapIntervals.define(2500,  "2.5 kHz", 2500.0);
        snapIntervals.define(5000,  "5 kHz",   5000.0);
        snapIntervals.define(10000, "10 kHz",   10000.0);
        snapIntervals.define(12500, "12.5 kHz", 12500.0);
        snapIntervals.define(25000, "25 kHz",   25000.0);

        // FolderSelect needs a default path; SDR++ config root is a sane choice.
        lrrpFolderSelect = std::make_unique<FolderSelect>(core::args["root"].s());

        loadSettings();

        // ---- register the output audio stream with SDR++'s sink manager ----
        // This is done ONCE for the lifetime of the module instance: it
        // persists across enable/disable cycles, mirroring how the radio
        // module and m17_decoder handle their audio.
        srChangeHandler.ctx = this;
        srChangeHandler.handler = sampleRateChangeHandler;
        outAudioStream.init(&outAudio, &srChangeHandler, SDRPP_OUT_SR);
        sigpath::sinkManager.registerStream(name, &outAudioStream);
        outAudioStream.start();
        currentSinkRate = SDRPP_OUT_SR;

        // ---- LRRP TCP sender to the Django map server ----------------------
        // The worker thread is started here so the toggle is instant, but it
        // stays idle (no socket, no connection attempt) until setEnabled(true)
        // is called. This is the loaded user setting, so SDR++ never opens
        // a TCP connection unless the user explicitly asked for it.
        tcpSender.configure(tcpHost, tcpPort);
        tcpSender.start();
        tcpSender.setEnabled(tcpEnabled);

        // ---- Restore the trunking role if any was persisted -----------
        if (channelType == ChannelType::CC) { startRigServer(); }
        else if (channelType == ChannelType::VC) { startRigClient(); }

        // ---- build VFO + DSP and start the decoder (initial enable) --------
        startupChain(/*useWaterfallBw=*/false);

        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~DSDDecoderModule() {
        gui::menu.removeEntry(name);
        // Tear DSP/decoder down BEFORE unregistering the sink stream so any
        // in-flight swap() unblocks cleanly.
        if (enabled) { teardownChain(); }
        stopRigServer();
        stopRigClient();
        tcpSender.stop();
        outAudioStream.stop();
        sigpath::sinkManager.unregisterStream(name);
    }

    void postInit() {}

    // SDR++ calls enable()/disable() ONLY on user toggle, never right after
    // construction. The module is alive and decoding by the time SDR++ might
    // call enable().
    void enable()  { startupChain(/*useWaterfallBw=*/true);  }
    void disable() { teardownChain(); }
    bool isEnabled() { return enabled; }

private:
    // ================= setup / teardown =============================
    //
    // The DSP blocks (fm, audioSink) are kept as unique_ptr<> so they are
    // *fresh* on every enable cycle. Reusing the same dsp::demod::FM
    // instance across init() calls leaked filter taps in older builds and
    // was the root cause of the on/off crash.
    //
    void startupChain(bool useWaterfallBw) {
        // VFO
        double offset = 0.0;
        if (useWaterfallBw) {
            double bw = gui::waterfall.getBandwidth();
            offset = std::clamp<double>(0.0, -bw / 2.0, bw / 2.0);
        }
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER,
                                            offset, vfoBandwidth, DSD_INPUT_SR,
                                            6250, 25000, false);
        vfo->setSnapInterval(snapIntervals.value(snapId));

        // FM discriminator → mono float audio at 48 kHz
        fm = std::make_unique<dsp::demod::FM<float>>();
        fm->init(vfo->output, DSD_INPUT_SR, vfoBandwidth, true);

        // Handler that captures the audio into a PCM ring for the writer thread.
        audioSink = std::make_unique<dsp::sink::Handler<float>>();
        audioSink->init(&fm->out, audioHandler, this);

        fm->start();
        audioSink->start();

        startDecoder();
        enabled = true;
    }

    void teardownChain() {
        if (!enabled) { return; }
        // Set enabled=false RIGHT AWAY (atomic store). Any in-flight rigctl
        // callback running on a server thread checks !enabled and returns
        // early, so it cannot race with the VFO deletion below and end up
        // calling tuner::tune / vfoManager on a VFO that is being destroyed.
        enabled = false;
        // Also flip the trunking state machine to Inactive so the GUI
        // doesn't briefly show a half-alive CC/VC state during teardown.
        rigState = RigState::Inactive;

        stopDecoder();
        if (audioSink) { audioSink->stop(); audioSink.reset(); }
        if (fm)        { fm->stop();        fm.reset();        }
        if (vfo)       { sigpath::vfoManager.deleteVFO(vfo); vfo = nullptr; }
    }

    // ================= subprocess lifecycle =========================

    std::vector<std::string> buildArgs(uint16_t udpPort) {
        std::vector<std::string> a;
        a.push_back(exePath.empty() ? std::string("dsd-fme") : exePath);
        a.push_back("-i"); a.push_back("-");                              // PCM on stdin
        a.push_back("-o");                                                // audio over UDP
        a.push_back("udp:127.0.0.1:" + std::to_string(udpPort));

        std::string flag = modes.value(modeId);
        if (!flag.empty()) { a.push_back(flag); }

        if (invertDMR)  { a.push_back("-xr"); }
        if (invertDPMR) { a.push_back("-xd"); }
        if (payloadLog) { a.push_back("-Z"); }

        // LRRP / GPS to file inside the chosen folder; we tail this file.
        a.push_back("-L"); a.push_back(lrrpFilePath());

        if (!encKey.empty()) { a.push_back("-H"); a.push_back(encKey); }

        // Trunking. Only the CONTROL CHANNEL instance feeds dsd-fme's
        // trunking flags: -T enables CC tracking and -U points dsd-fme at
        // our embedded rigctl server (which we then mirror to VC clients).
        // VC instances just decode voice for whatever freq SDR++ is tuned
        // to — they don't need dsd-fme to know anything about trunking.
        if (channelType == ChannelType::CC) {
            a.push_back("-U"); a.push_back(std::to_string(rigctlPort));
            a.push_back("-T");
            if (!trunkChanMap.empty()) { a.push_back("-C"); a.push_back(trunkChanMap); }
            if (!trunkGroups.empty())  { a.push_back("-G"); a.push_back(trunkGroups);  }
        }

        std::istringstream iss(extraArgs);
        std::string tok;
        while (iss >> tok) { a.push_back(tok); }
        return a;
    }

    std::string lrrpFilePath() const {
        std::string dir = lrrpFolderSelect && lrrpFolderSelect->pathIsValid()
                          ? lrrpFolderSelect->path
                          : core::args["root"].s();
        return dir + "/dsd_lrrp_" + name + ".txt";
    }

    void startDecoder() {
        if (decoderRunning) { return; }

        // Reset buffers / state.
        { std::lock_guard<std::mutex> lck(pcmMtx); pcmQueue.clear(); }
        droppedSamples = 0;

        // Truncate the LRRP file and reset tail offset.
        { std::ofstream(lrrpFilePath(), std::ios::trunc); }
        lrrpOffset = 0;

        // Open the UDP receive socket BEFORE spawning the child, so the child
        // never sends into a closed port.
        if (!openUdp()) {
            onConsoleLine("[dsd_decoder] failed to open UDP audio socket; audio output disabled");
            // continue anyway: stdin / stdout still work for decoding text
        }

        proc = std::make_unique<DsdProcess>();
        bool ok = proc->start(buildArgs(udpPort),
                              [this](const std::string& line) { this->onConsoleLine(line); });
        if (!ok) {
            onConsoleLine(std::string("[dsd_decoder] ") + proc->lastErr());
            proc.reset();
            closeUdp();
            return;
        }

        decoderRunning = true;

        // The output stream may have been "writer-stopped" by the previous
        // teardown; clear that flag so swap() works on this run.
        outAudio.clearWriteStop();

        writerThread = std::thread(&DSDDecoderModule::writerLoop, this);
        lrrpThread   = std::thread(&DSDDecoderModule::lrrpLoop,   this);
        if (udpSock >= 0) {
            udpThread = std::thread(&DSDDecoderModule::udpLoop, this);
        }
    }

    void stopDecoder() {
        if (!decoderRunning && !proc) { return; }
        decoderRunning = false;

        // Kill the subprocess FIRST. This closes its pipe FDs from our side
        // (and SIGTERMs/SIGKILLs the child), which unblocks any of OUR worker
        // threads that may be stuck in write(stdin) or read(stdout) on the
        // child's pipes. Doing this AFTER the joins below would deadlock if
        // dsd-fme is unresponsive.
        if (proc) { proc->stop(); }

        // Wake our remaining waiting threads.
        pcmCv.notify_all();      // writer thread blocked on pcmCv.wait
        closeUdp();              // UDP recv() returns
        outAudio.stopWriter();   // unblocks any pending swap() in UDP thread

        if (writerThread.joinable()) { writerThread.join(); }
        if (lrrpThread.joinable())   { lrrpThread.join();   }
        if (udpThread.joinable())    { udpThread.join();    }

        if (proc) { proc.reset(); }
    }

    void restartDecoder() {
        if (!enabled) { return; }
        stopDecoder();
        startDecoder();
    }

    // ================= UDP audio out from dsd-fme ===================

    bool openUdp() {
#ifndef _WIN32
        udpSock = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (udpSock < 0) { return false; }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // let the OS pick a free port (avoids collisions between instances)
        if (::bind(udpSock, (sockaddr*)&addr, sizeof(addr)) != 0) {
            ::close(udpSock); udpSock = -1; return false;
        }
        socklen_t len = sizeof(addr);
        if (::getsockname(udpSock, (sockaddr*)&addr, &len) != 0) {
            ::close(udpSock); udpSock = -1; return false;
        }
        udpPort = ntohs(addr.sin_port);

        // Generous receive buffer for vocoder bursts.
        int rcvbuf = 256 * 1024;
        setsockopt(udpSock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        // Short timeout so the thread can exit promptly when we close the socket.
        timeval tv{1, 0};
        setsockopt(udpSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        return true;
#else
        return false;
#endif
    }

    void closeUdp() {
#ifndef _WIN32
        if (udpSock >= 0) { ::shutdown(udpSock, SHUT_RDWR); ::close(udpSock); udpSock = -1; }
#endif
    }

    void udpLoop() {
#ifndef _WIN32
        // 4096 mono int16 samples per UDP read max (8 kB) -> 24576 stereo samples
        // after 6x upsample. Well under STREAM_BUFFER_SIZE.
        static constexpr int MAX_IN  = 4096;
        std::vector<int16_t> pcm(MAX_IN);
        float prev = 0.0f; // last sample of the previous block, for interp continuity

        while (decoderRunning && udpSock >= 0) {
            ssize_t r = ::recv(udpSock, pcm.data(), MAX_IN * sizeof(int16_t), 0);
            if (r <= 0) { continue; } // timeout / closed -> loop or exit
            int nIn = (int)(r / sizeof(int16_t));
            if (nIn <= 0) { continue; }

            // Upsample 8k -> 48k (linear interpolation) and stereoize into outAudio.writeBuf.
            // For each input sample s[i], emit 6 stereo samples interpolating from prev -> s[i].
            const int nOut = nIn * UPSAMPLE_RATIO;
            dsp::stereo_t* w = outAudio.writeBuf;
            for (int i = 0; i < nIn; i++) {
                float cur = pcm[i] / 32768.0f;
                for (int j = 0; j < UPSAMPLE_RATIO; j++) {
                    float t = (float)j / (float)UPSAMPLE_RATIO;
                    float s = prev + (cur - prev) * t;
                    w[i * UPSAMPLE_RATIO + j] = { s, s };
                }
                prev = cur;
            }
            if (!outAudio.swap(nOut)) { break; } // writer stopped
        }
#endif
    }

    static void sampleRateChangeHandler(float sr, void* ctx) {
        // The sink may choose its own internal rate; SDR++ tells us via this
        // callback. We don't actually need to react: we always feed 48 kHz and
        // SDR++ resamples downstream. We keep the rate for display only.
        DSDDecoderModule* _this = (DSDDecoderModule*)ctx;
        _this->currentSinkRate = sr;
    }

    // ================= internal RIGCTL server + CC/VC state machine ======
    //
    // We embed a small RIGCTL TCP server (see rigctl_internal.h). dsd-fme is
    // spawned with `-T -U <port>` pointing at us, so every "F <freq>" voice
    // grant goes through this module — letting us not only retune SDR++ but
    // also track our exact state: CC (parked on control channel, waiting for
    // grants) vs VC (following a voice channel).
    //
    // The CC frequency is captured automatically when the user enables
    // trunking (snapshot of the current VFO freq) and can be overridden by
    // hand. If dsd-fme's last call activity is older than autoReturnSec, we
    // proactively re-tune back to the CC — useful because some systems don't
    // emit an explicit "release" RIGCTL command and just go silent.

    enum class ChannelType { None = 0, CC = 1, VC = 2 };
    enum class RigState    { Inactive, CC, VC };

    struct GrantEvent {
        std::time_t ts    = 0;   // when the grant arrived
        std::time_t endTs = 0;   // when we returned to CC (0 = still active)
        double      freq  = 0.0;
        std::string tg;          // best-effort, joined from the call parser
        std::string src;
    };

    // Called from a rigctl SERVER client thread when dsd-fme (or any other
    // rigctl client) sends us a "F <freq>". In CC mode we DO NOT retune our
    // own VFO — we stay parked on the control channel — but we broadcast the
    // grant to every other rigctl client connected to us (i.e. the VC
    // instances) so they can follow the voice.
    bool onRigSetFreq(double f) {
        if (!enabled || !vfo) { return false; }
        if (channelType != ChannelType::CC) { return false; }

        std::time_t now = std::time(nullptr);
        const double EPS = 100.0; // Hz tolerance when comparing to ccFreq

        bool toCC = (ccFreq > 0.0) && std::abs(f - ccFreq) < EPS;

        if (toCC) {
            // dsd-fme tells us to return to CC after a call ends. We're
            // already on the CC by design — just close any open grant. We
            // still broadcast the F so VCs can mark the call as ended too.
            std::lock_guard<std::mutex> lck(grantMtx);
            if (!grantHistory.empty() && grantHistory.back().endTs == 0) {
                grantHistory.back().endTs = now;
            }
            currentVCFreq = 0.0;
            // No state change: CC stays CC (we never become VC ourselves).
        } else {
            // A voice grant. Open a new entry and broadcast to VCs.
            std::lock_guard<std::mutex> lck(grantMtx);
            if (!grantHistory.empty() && grantHistory.back().endTs == 0) {
                grantHistory.back().endTs = now;
            }
            GrantEvent g;
            g.ts   = now;
            g.freq = f;
            grantHistory.push_back(g);
            while (grantHistory.size() > GRANT_HISTORY_MAX) { grantHistory.pop_front(); }
            grantCount++;
            currentVCFreq       = f;
            vcStartTime         = now;
            lastVoiceActivityTs = now;
        }

        // Forward to every connected VC client. broadcastSetFreq is
        // best-effort and non-blocking.
        rigServer.broadcastSetFreq(f);
        return true;
    }

    // Called from the rigctl CLIENT worker thread, when our CC peer sends us
    // a "F <freq>". VC mode retunes ITS OWN VFO via the SDR++ tuner; CC and
    // None ignore the callback.
    void onRigctlClientSetFreq(double f) {
        if (!enabled || !vfo) { return; }
        if (channelType != ChannelType::VC) { return; }
        if (f <= 0.0) { return; }

        tuner::tune(tuner::TUNER_MODE_NORMAL, name, f);

        std::time_t now = std::time(nullptr);
        std::lock_guard<std::mutex> lck(grantMtx);
        if (!grantHistory.empty() && grantHistory.back().endTs == 0) {
            grantHistory.back().endTs = now;
        }
        GrantEvent g;
        g.ts   = now;
        g.freq = f;
        grantHistory.push_back(g);
        while (grantHistory.size() > GRANT_HISTORY_MAX) { grantHistory.pop_front(); }
        grantCount++;
        currentVCFreq       = f;
        vcStartTime         = now;
        lastVoiceActivityTs = now;
    }

    double getCurrentVfoFreq() {
        double f = gui::waterfall.getCenterFrequency();
        if (sigpath::vfoManager.vfoExists(name)) {
            f += sigpath::vfoManager.getOffset(name);
        }
        return f;
    }

    // Called from the GUI thread at frame rate. Cheap, branches early.
    // CC and VC have very different "tick" semantics:
    //   * CC: nothing to do (we never auto-retune).
    //   * VC: nothing to do either — per spec, the VC stays on the last
    //     received freq until the next grant arrives. No auto-mute, no
    //     auto-return.
    void rigStateTick() { /* intentionally empty for the split CC/VC model */ }

    void startRigServer() {
        rigServer.onSetFreq      = [this](double f) { return onRigSetFreq(f); };
        rigServer.getCurrentFreq = [this]           { return getCurrentVfoFreq(); };
        rigServer.getCurrentBw   = [this]           { return (double)vfoBandwidth; };
        if (rigServer.start(rigctlPort)) {
            // Snapshot the current VFO freq as the CC frequency, unless the
            // user has already overridden it.
            if (ccFreq <= 0.0) { ccFreq = getCurrentVfoFreq(); }
            rigState = RigState::CC;
        }
    }

    void stopRigServer() {
        rigServer.stop();
        rigState      = RigState::Inactive;
        currentVCFreq = 0.0;
    }

    void startRigClient() {
        rigClient.onSetFreq = [this](double f) { onRigctlClientSetFreq(f); };
        rigClient.configure(vcServerHost, vcServerPort);
        rigClient.start();
        rigClient.setEnabled(true);
        rigState = RigState::VC;
    }

    void stopRigClient() {
        rigClient.setEnabled(false);
        rigClient.stop();
        rigState      = RigState::Inactive;
        currentVCFreq = 0.0;
    }

    // Apply the channel type, starting/stopping the right side(s) and
    // restarting dsd-fme so the right argv (presence/absence of -T -U) is
    // picked up. Safe to call repeatedly; transitions to the same type are
    // no-ops.
    void applyChannelType(ChannelType newType) {
        if (newType == channelType) { return; }
        // Tear down whatever the previous type had spun up.
        switch (channelType) {
            case ChannelType::CC: stopRigServer(); break;
            case ChannelType::VC: stopRigClient(); break;
            default: break;
        }
        channelType = newType;
        // Bring up the new role.
        switch (channelType) {
            case ChannelType::CC: startRigServer(); break;
            case ChannelType::VC: startRigClient(); break;
            default:              rigState = RigState::Inactive; break;
        }
        // dsd-fme command-line differs for CC (gets -T -U) vs VC/None: restart.
        restartDecoder();
    }


    // ================= DSP -> PCM queue (INPUT to dsd-fme) ==========

    static void audioHandler(float* data, int count, void* ctx) {
        DSDDecoderModule* _this = (DSDDecoderModule*)ctx;
        if (!_this->decoderRunning) { return; }

        const float g = _this->inputGain * 32767.0f;
        std::lock_guard<std::mutex> lck(_this->pcmMtx);
        if (_this->pcmQueue.size() > PCM_QUEUE_MAX) {
            size_t drop = _this->pcmQueue.size() - PCM_QUEUE_MAX;
            _this->pcmQueue.erase(_this->pcmQueue.begin(), _this->pcmQueue.begin() + drop);
            _this->droppedSamples += drop;
        }
        for (int i = 0; i < count; i++) {
            float v = data[i] * g;
            v = std::clamp(v, -32768.0f, 32767.0f);
            _this->pcmQueue.push_back((int16_t)v);
        }
        _this->pcmCv.notify_one();
    }

    void writerLoop() {
        std::vector<int16_t> local;
        local.reserve(8192);
        while (decoderRunning) {
            {
                std::unique_lock<std::mutex> lck(pcmMtx);
                pcmCv.wait(lck, [this] { return !pcmQueue.empty() || !decoderRunning; });
                if (!decoderRunning && pcmQueue.empty()) { break; }
                local.assign(pcmQueue.begin(), pcmQueue.end());
                pcmQueue.clear();
            }
            if (!local.empty()) {
                if (!proc || !proc->writeSamples(local.data(), local.size())) {
                    onConsoleLine("[dsd_decoder] dsd-fme stdin closed (child exited?). Decoder stopped.");
                    decoderRunning = false;
                    break;
                }
            }
        }
    }

    // ================= console capture ==============================

    static std::string stripAnsi(const std::string& in) {
        std::string out;
        out.reserve(in.size());
        for (size_t i = 0; i < in.size(); i++) {
            unsigned char c = (unsigned char)in[i];
            if (c == 0x1B) { // ESC: skip CSI "ESC [ ... <final>"
                if (i + 1 < in.size() && in[i + 1] == '[') {
                    i += 2;
                    while (i < in.size() && !((in[i] >= '@' && in[i] <= '~'))) { i++; }
                    continue;
                }
                continue;
            }
            if (c == '\r') { continue; }
            // Drop bytes outside printable ASCII. dsd-fme's startup banner
            // is built from Unicode box-drawing / block characters
            // (U+2500..U+259F, multi-byte UTF-8). The default ImGui font
            // has no glyphs for them and renders each byte as "?", which
            // clutters the console. Tab is preserved for alignment.
            if (c < 0x20 && c != '\t') { continue; }
            if (c > 0x7E)              { continue; }
            out.push_back((char)c);
        }
        return out;
    }

    void onConsoleLine(const std::string& rawLine) {
        std::string line = stripAnsi(rawLine);

        // Skip lines that became empty (or whitespace-only) after stripping.
        // Typically the banner art ends up here once box-drawing chars are
        // removed.
        bool blank = true;
        for (char c : line) { if (c != ' ' && c != '\t') { blank = false; break; } }
        if (blank) { return; }

        // Update the structured call history from this line (best-effort).
        parseCallInfo(line);
        std::lock_guard<std::mutex> lck(consoleMtx);
        consoleLines.push_back({ std::time(nullptr), line });
        while (consoleLines.size() > CONSOLE_MAX_LINES) { consoleLines.pop_front(); }
        consoleDirty = true;
    }

    // ================= call history parser ==========================

    // Lines that describe data exchanges (SMS, GPS pings, data grants, PDUs,
    // location reports, etc.) carry TG / Src too, but the user explicitly
    // does NOT want those in the call history. We keep only lines that look
    // like real voice activity (or a voice channel grant). Detection is
    // intentionally case-insensitive: dsd-fme's wording isn't 100% stable
    // across versions.
    static bool isVoiceLine(const std::string& line) {
        // Lower-case once, then plain substring checks (cheaper than regex).
        std::string lo = line;
        std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);

        // (1) Definitely data — drop immediately.
        static const char* dataKw[] = {
            "td_grant",                // DMR Talkgroup Data Channel Grant
            "pd_grant",                // DMR Private Data Grant
            "data channel grant",
            "data call",
            "data header",
            "data block",
            "confirmed data",
            "unconfirmed data",
            "short data",
            "short message",
            " sms ",
            "sms header",
            " pdu ",
            "pdu data",
            "gps location",
            "gps position",
            "location update",
            "lrrp",
            "dmra",                    // Motorola DMR-A location/data
            "status pdu",
            "ars header",              // Automatic Registration Service
            "telegram",                // Hytera proprietary data
            "data sync",
            nullptr
        };
        for (int i = 0; dataKw[i]; i++) {
            if (lo.find(dataKw[i]) != std::string::npos) { return false; }
        }

        // (2) Require at least one explicit voice marker.
        static const char* voiceKw[] = {
            "vc ",                     // DMR "VC Slot N ..." voice frame
            "voice call",
            "voice channel",
            "voice frame",
            "tv_grant",                // Talkgroup VOICE Channel Grant (DMR)
            "pv_grant",                // Private Voice Channel Grant
            "group call",              // DSDPlus / older dsd-fme style
            "private call",
            "ldu1", "ldu2",            // P25 voice logical data units
            "vch",
            "vframe",
            // DMR repeater/duplex voice-burst markers. dsd-fme prints these
            // on Sync lines and on the SLOT data line; in duplex the SLOT
            // line often has no "Group Call"/"Private Call" suffix, so we
            // must recognise the burst type itself.
            "vlc",                     // Voice LC header
            "vc*",                     // voice superframe burst
            "tlc",                     // Terminator with LC (end of voice tx)
            nullptr
        };
        for (int i = 0; voiceKw[i]; i++) {
            if (lo.find(voiceKw[i]) != std::string::npos) { return true; }
        }

        // (3) Fallback for DMR: a "SLOT n ... TGT=" line that survived the
        // data-keyword filter above is a voice frame (data frames are
        // caught by "data header" / "pdu" / "data block" etc.). This covers
        // duplex/repeater output where the call-type suffix is absent.
        if (lo.find("slot") != std::string::npos &&
            lo.find("tgt") != std::string::npos) {
            return true;
        }
        return false;
    }

    void parseCallInfo(const std::string& line) {
        // Always: even on a non-voice line, capture protocol + CC context so
        // the next voice-line can inherit them (typical DMR output: protocol
        // and CC on a Sync line, then TG/Src on the next line).
        static const std::regex reProto(
            R"(\b(P25P1|P25P2|NXDN48|NXDN96|YSF|dPMR|EDACS|M17|X2-TDMA|ProVoice|DMR)\b)");
        static const std::regex reCC0(
            R"((?:Color\s*Code|\bCC)\s*[:=]?\s*\$?([0-9A-Fa-f]+))", std::regex::icase);
        std::smatch m;
        if (std::regex_search(line, m, reProto)) {
            std::lock_guard<std::mutex> lck(callsMtx);
            lastSeenProto = m[1].str();
        }
        if (std::regex_search(line, m, reCC0)) {
            std::lock_guard<std::mutex> lck(callsMtx);
            lastSeenCC = m[1].str();
            // The Sync line is the AUTHORITATIVE source for Color Code.
            // dsd-fme often emits CC=15 (the "unknown" 4-bit placeholder)
            // during sync acquisition and then corrects to the real CC
            // once stable. So whenever we learn a new CC from a sync line,
            // overwrite the CC of any currently-open call: at any moment
            // the latest sync value is the most credible one.
            for (auto& c : calls) {
                if (!c.closed) { c.cc = lastSeenCC; }
            }
        }

        // Now the voice-only gate: skip everything else (SMS, GPS, data
        // grants, PDUs, location pings, ...).
        if (!isVoiceLine(line)) { return; }

        // Field extractors. Numbers may be prefixed with $ in P25 (hex).
        // TGT is dsd-fme's preferred form on DMR voice-frame lines
        // ("SLOT 1 TGT=20806 SRC=2081371 Group Call"). All field regexes
        // are case-insensitive so they cope with both "SLOT" and "Slot",
        // "SRC" and "Src", "TGT" and "tgt", etc.
        static const std::regex reTG (R"((?:TGT|TG(?:ID)?|Talkgroup|Target)\s*[:=]?\s*\$?(\d+))",
                                      std::regex::icase);
        static const std::regex reSrc(R"((?:Src|SRC|RID|Source)\s*[:=]?\s*\$?(\d+))",
                                      std::regex::icase);
        static const std::regex reSlotBr (R"(\[slot(\d)\])",                std::regex::icase);
        static const std::regex reSlotW  (R"(\bslot\s*[:=]?\s*(\d)\b)",      std::regex::icase);
        static const std::regex reSlotTS (R"((?:^|\s)TS\s*[:=]\s*(\d))",     std::regex::icase);
        static const std::regex reCC (R"((?:Color\s*Code|\bCC)\s*[:=]?\s*\$?([0-9A-Fa-f]+))",
                                      std::regex::icase);
        static const std::regex reNAC(R"(NAC\s*[:=]?\s*\$?([0-9A-Fa-f]+))", std::regex::icase);
        static const std::regex reENC(R"(\bEncrypted\b|ENC\s*[:=]\s*(?!Clear\b)\S+)", std::regex::icase);

        std::string tg, src, cc, nac;
        int slot = 0;
        bool enc = false;

        if (std::regex_search(line, m, reTG))     { tg  = m[1].str(); }
        if (std::regex_search(line, m, reSrc))    { src = m[1].str(); }
        if (std::regex_search(line, m, reSlotBr)) { slot = std::atoi(m[1].str().c_str()); }
        else if (std::regex_search(line, m, reSlotW))  { slot = std::atoi(m[1].str().c_str()); }
        else if (std::regex_search(line, m, reSlotTS)) { slot = std::atoi(m[1].str().c_str()); }
        if (std::regex_search(line, m, reCC))     { cc  = m[1].str(); }
        if (std::regex_search(line, m, reNAC))    { nac = m[1].str(); }
        if (std::regex_search(line, m, reENC))    { enc = true; }

        // Need at least TG or Src to consider this a call-relevant line.
        if (tg.empty() && src.empty()) { return; }

        std::time_t now = std::time(nullptr);
        std::lock_guard<std::mutex> lck(callsMtx);

        // Find the most recent open call on this slot.
        CallEntry* current = nullptr;
        for (auto it = calls.rbegin(); it != calls.rend(); ++it) {
            if (!it->closed && it->slot == slot) { current = &(*it); break; }
        }

        // Decide whether this voice frame still belongs to the current call.
        // Two events imply the previous transmission has ended:
        //   (a) the TG has changed (different group/private conversation),
        //   (b) the TG is the same but the SRC changed (someone else picked
        //       up the mic on the same TG — e.g. 50 spoke, then 51 answers).
        // Both must close the existing entry and open a new one so the GUI
        // shows the right active-call timer and source.
        bool tgMismatch  = current && !tg.empty()  && !current->tg.empty()  && current->tg  != tg;
        bool srcMismatch = current && !src.empty() && !current->src.empty() && current->src != src;
        if (current && (tgMismatch || srcMismatch)) {
            current->closed = true;
            current->endTs  = current->lastActivityTs;
            current = nullptr;
        }

        if (!current) {
            // Open a new call entry.
            CallEntry e;
            e.startTs        = now;
            e.lastActivityTs = now;
            e.protocol       = lastSeenProto;
            e.tg             = tg;
            e.src            = src;
            e.slot           = slot;
            e.cc             = cc.empty() ? lastSeenCC : cc;
            e.nac            = nac;
            e.enc            = enc;
            calls.push_back(std::move(e));
            while (calls.size() > CALLS_MAX_ROWS) { calls.pop_front(); }
        } else {
            // Augment the current call with new fields we just learned.
            current->lastActivityTs = now;
            if (!lastSeenProto.empty() && current->protocol.empty()) { current->protocol = lastSeenProto; }
            if (current->tg.empty()  && !tg.empty())  { current->tg  = tg; }
            if (current->src.empty() && !src.empty()) { current->src = src; }
            // CC: always converge to the most recent known value
            // (line wins if it carries one, else inherit the latest sync).
            std::string newCC = !cc.empty() ? cc : lastSeenCC;
            if (!newCC.empty()) { current->cc = newCC; }
            if (current->nac.empty() && !nac.empty()) { current->nac = nac; }
            if (enc) { current->enc = true; }
        }

        // Trunking: stamp the voice-activity timestamp so the auto-return-to-CC
        // logic knows we're still hearing voice on this VC. Also opportunistically
        // backfill TG/SRC on the most recent open grant so the UI can label it.
        if (rigState == RigState::VC) {
            lastVoiceActivityTs = now;
            std::lock_guard<std::mutex> lck2(grantMtx);
            if (!grantHistory.empty() && grantHistory.back().endTs == 0) {
                if (grantHistory.back().tg.empty()  && !tg.empty())  { grantHistory.back().tg  = tg; }
                if (grantHistory.back().src.empty() && !src.empty()) { grantHistory.back().src = src; }
            }
        }
    }

    void sweepStaleCalls() {
        std::time_t now = std::time(nullptr);
        std::lock_guard<std::mutex> lck(callsMtx);
        for (auto& c : calls) {
            if (!c.closed && now - c.lastActivityTs > CALL_HANG_SEC) {
                c.closed = true;
                c.endTs  = c.lastActivityTs;
            }
        }
    }

    void saveCallLogCsv() {
        std::string path = core::args["root"].s() + "/dsd_calls_" + name + ".csv";
        std::ofstream f(path, std::ios::trunc);
        if (!f) { return; }
        f << "start_iso,end_iso,duration_s,protocol,slot,tg,src,cc,nac,enc,active\n";
        std::lock_guard<std::mutex> lck(callsMtx);
        for (const auto& c : calls) {
            char start[24], end[24];
            std::tm tmv;
#ifdef _WIN32
            localtime_s(&tmv, &c.startTs);
            std::strftime(start, sizeof(start), "%Y-%m-%dT%H:%M:%S", &tmv);
            std::time_t et = c.closed ? c.endTs : c.lastActivityTs;
            localtime_s(&tmv, &et);
            std::strftime(end, sizeof(end), "%Y-%m-%dT%H:%M:%S", &tmv);
#else
            localtime_r(&c.startTs, &tmv);
            std::strftime(start, sizeof(start), "%Y-%m-%dT%H:%M:%S", &tmv);
            std::time_t et = c.closed ? c.endTs : c.lastActivityTs;
            localtime_r(&et, &tmv);
            std::strftime(end, sizeof(end), "%Y-%m-%dT%H:%M:%S", &tmv);
#endif
            int dur = (int)((c.closed ? c.endTs : c.lastActivityTs) - c.startTs);
            f << start << "," << end << "," << dur << "," << c.protocol << ","
              << (c.slot ? std::to_string(c.slot) : "") << ","
              << c.tg << "," << c.src << "," << c.cc << "," << c.nac << ","
              << (c.enc ? "1" : "0") << "," << (c.closed ? "0" : "1") << "\n";
        }
        flog::info("dsd_decoder: saved call log to {}", path);
    }

    // ================= LRRP file tail ===============================

    void lrrpLoop() {
        std::string path = lrrpFilePath();
        while (decoderRunning) {
            std::ifstream f(path, std::ios::binary);
            if (f) {
                f.seekg(0, std::ios::end);
                std::streamoff end = f.tellg();
                if (end > lrrpOffset) {
                    f.seekg(lrrpOffset, std::ios::beg);
                    std::string line;
                    while (std::getline(f, line)) {
                        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
                        if (!line.empty()) { parseLrrp(line); }
                    }
                    lrrpOffset = end;
                }
            }
            for (int i = 0; i < 10 && decoderRunning; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        }
    }

    void parseLrrp(const std::string& line) {
        // dsd-fme writes one row per LRRP/GPS fix to opts->lrrp_out_file, in
        // the DSDPlus.LRRP tab-separated layout (see src/dsd_gps.c and
        // src/dmr_pdu.c in lwvmobile/dsd-fme):
        //
        //   <YYYY/MM/DD>\t<HH:MM:SS>\t<source_8digits>\t<lat>\t<lon>\t<spd>\t<dir>\t\n
        //
        // The trailing space after some fields (`%d\t ` in dmr code) is
        // tolerated by trim. lat/lon use '.' as decimal separator and may be
        // negative. We accept any line that *parses* into >= 5 columns, the
        // first column matching a date pattern and columns 4/5 numeric.
        LrrpEntry e;
        e.raw = stripAnsi(line);

        // Split on TAB.
        std::vector<std::string> cols;
        {
            std::string cur;
            for (char c : e.raw) {
                if (c == '\t') { cols.push_back(cur); cur.clear(); }
                else            { cur.push_back(c); }
            }
            if (!cur.empty()) { cols.push_back(cur); }
        }
        auto trim = [](std::string& s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\r')) { s.erase(s.begin()); }
            while (!s.empty() && (s.back()  == ' ' || s.back()  == '\r')) { s.pop_back(); }
        };
        for (auto& c : cols) { trim(c); }

        bool parsed = false;
        if (cols.size() >= 5) {
            static const std::regex reDate(R"(^\d{4}[-/]\d{1,2}[-/]\d{1,2}$)");
            static const std::regex reTime(R"(^\d{1,2}:\d{2}:\d{2}$)");
            static const std::regex reNum (R"(^-?\d+(\.\d+)?$)");
            if (std::regex_match(cols[0], reDate) &&
                std::regex_match(cols[1], reTime) &&
                std::regex_match(cols[3], reNum)  &&
                std::regex_match(cols[4], reNum))
            {
                // Strip the leading zeros on the source ID so the UI shows
                // a "natural" RID (e.g. 2081371 rather than 02081371).
                std::string src = cols[2];
                while (src.size() > 1 && src.front() == '0') { src.erase(src.begin()); }
                e.time   = cols[1];
                e.source = src;
                e.lat    = cols[3];
                e.lon    = cols[4];
                // Columns 5 (speed km/h) and 6 (direction units of 2°) are
                // optional and may carry a trailing-space "0\t " from
                // dsd-fme's printf. Trim already cleaned that.
                if (cols.size() > 5 && std::regex_match(cols[5], reNum)) { e.speed = cols[5]; }
                if (cols.size() > 6 && std::regex_match(cols[6], reNum)) { e.dir   = cols[6]; }
                parsed   = true;
            }
        }

        // Build local date/time for the JSON.
        char tbuf[16], dbuf[16];
        std::time_t t = std::time(nullptr);
        std::tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmv);
        std::strftime(dbuf, sizeof(dbuf), "%Y-%m-%d", &tmv);
        std::string date = dbuf;
        if (e.time.empty()) { e.time = tbuf; }

        // Lines that don't parse as the canonical format are still kept in
        // the LRRP tab (raw column only) for visibility, but never sent over
        // TCP — we won't push half-baked positions to the map.
        if (parsed && tcpEnabled && !e.lat.empty() && !e.lon.empty()) {
            sendLrrpJson(e, date);
        }

        std::lock_guard<std::mutex> lck(lrrpMtx);
        lrrp.push_back(std::move(e));
        while (lrrp.size() > LRRP_MAX_ROWS) { lrrp.pop_front(); }
    }

    static std::string jsonEscape(const std::string& s) {
        std::string o;
        o.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
                case '"':  o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n";  break;
                case '\r': o += "\\r";  break;
                case '\t': o += "\\t";  break;
                default:
                    if ((unsigned char)c < 0x20) { /* drop control chars */ }
                    else { o += c; }
            }
        }
        return o;
    }

    void sendLrrpJson(const LrrpEntry& e, const std::string& date) {
        // name: prefer the source RID; fall back to a generic label so the
        // object still has a stable identity on the map.
        std::string nm = e.source.empty() ? ("LRRP-" + name) : ("RID " + e.source);

        // info: keep the raw decoded line so nothing is lost on the map side.
        std::string info = jsonEscape(e.raw);

        std::string json = "{";
        json += "\"name\":\""   + jsonEscape(nm) + "\",";
        json += "\"date\":\""   + date + "\",";
        json += "\"time\":\""   + e.time + "\",";
        json += "\"lat\":"      + e.lat + ",";
        json += "\"lon\":"      + e.lon + ",";
        json += "\"type\":\"lrrp\",";
        if (e.speed.empty()) { json += "\"speed\":null,"; }
        else                  { json += "\"speed\":" + e.speed + ","; }
        if (!e.dir.empty())   { json += "\"dir\":" + e.dir + ","; }
        if (!e.source.empty()) { json += "\"source\":\"" + jsonEscape(e.source) + "\","; }
        json += "\"info\":\""   + info + "\"";
        json += "}";
        tcpSender.send(json);
    }

    // ================= settings persistence =========================

    void loadSettings() {
        config.acquire();
        if (!config.conf.contains(name)) { config.conf[name] = json::object(); }
        json& c = config.conf[name];
        if (c.contains("mode"))         { modeId       = std::max(0, modes.valueId(c["mode"].get<std::string>())); }
        if (c.contains("bandwidth"))    { vfoBandwidth = c["bandwidth"].get<float>(); }
        if (c.contains("inputGain"))    { inputGain    = c["inputGain"].get<float>(); }
        if (c.contains("invertDMR"))    { invertDMR    = c["invertDMR"].get<bool>(); }
        if (c.contains("invertDPMR"))   { invertDPMR   = c["invertDPMR"].get<bool>(); }
        if (c.contains("payloadLog"))   { payloadLog   = c["payloadLog"].get<bool>(); }
        if (c.contains("encKey"))       { encKey       = c["encKey"].get<std::string>(); }
        if (c.contains("extraArgs"))    { extraArgs    = c["extraArgs"].get<std::string>(); }
        if (c.contains("exePath"))      { exePath      = c["exePath"].get<std::string>(); }
        if (c.contains("snap"))         {
            int v = c["snap"].get<int>();
            int id = snapIntervals.valueId(v);
            if (id >= 0) { snapId = id; }
        }
        if (c.contains("lrrpFolder") && lrrpFolderSelect) {
            lrrpFolderSelect->setPath(c["lrrpFolder"].get<std::string>(), false);
        }
        if (c.contains("channelType")) {
            int t = c["channelType"].get<int>();
            if (t >= 0 && t <= 2) { channelType = (ChannelType)t; }
        }
        if (c.contains("vcServerHost")) { vcServerHost = c["vcServerHost"].get<std::string>(); }
        if (c.contains("vcServerPort")) { vcServerPort = c["vcServerPort"].get<int>(); }
        if (c.contains("rigctlPort"))   { rigctlPort    = c["rigctlPort"].get<int>(); }
        if (c.contains("trunkChanMap")) { trunkChanMap  = c["trunkChanMap"].get<std::string>(); }
        if (c.contains("trunkGroups"))  { trunkGroups   = c["trunkGroups"].get<std::string>(); }
        if (c.contains("ccFreq"))             { ccFreq            = c["ccFreq"].get<double>(); }
        if (c.contains("autoReturnEnabled")) { autoReturnEnabled = c["autoReturnEnabled"].get<bool>(); }
        if (c.contains("autoReturnSec"))     { autoReturnSec     = c["autoReturnSec"].get<float>(); }
        if (c.contains("tcpEnabled"))   { tcpEnabled    = c["tcpEnabled"].get<bool>(); }
        if (c.contains("tcpHost"))      { tcpHost       = c["tcpHost"].get<std::string>(); }
        if (c.contains("tcpPort"))      { tcpPort       = c["tcpPort"].get<int>(); }
        config.release();
    }

    void saveSettings() {
        config.acquire();
        config.conf[name]["mode"]       = modes.value(modeId);
        config.conf[name]["bandwidth"]  = vfoBandwidth;
        config.conf[name]["inputGain"]  = inputGain;
        config.conf[name]["invertDMR"]  = invertDMR;
        config.conf[name]["invertDPMR"] = invertDPMR;
        config.conf[name]["payloadLog"] = payloadLog;
        config.conf[name]["encKey"]     = encKey;
        config.conf[name]["extraArgs"]  = extraArgs;
        config.conf[name]["exePath"]    = exePath;
        config.conf[name]["snap"]       = snapIntervals.key(snapId);
        if (lrrpFolderSelect) { config.conf[name]["lrrpFolder"] = lrrpFolderSelect->path; }
        config.conf[name]["channelType"]  = (int)channelType;
        config.conf[name]["vcServerHost"] = vcServerHost;
        config.conf[name]["vcServerPort"] = vcServerPort;
        config.conf[name]["rigctlPort"]   = rigctlPort;
        config.conf[name]["trunkChanMap"] = trunkChanMap;
        config.conf[name]["trunkGroups"]  = trunkGroups;
        config.conf[name]["ccFreq"]             = ccFreq;
        config.conf[name]["autoReturnEnabled"] = autoReturnEnabled;
        config.conf[name]["autoReturnSec"]     = autoReturnSec;
        config.conf[name]["tcpEnabled"]   = tcpEnabled;
        config.conf[name]["tcpHost"]      = tcpHost;
        config.conf[name]["tcpPort"]      = tcpPort;
        config.release(true);
    }

    // ================= GUI ==========================================

    static void menuHandler(void* ctx) {
        DSDDecoderModule* _this = (DSDDecoderModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) { style::beginDisabled(); }

        // ---- decode mode ----
        ImGui::LeftLabel("Mode");
        ImGui::FillWidth();
        if (ImGui::Combo(CONCAT("##dsd_mode_", _this->name), &_this->modeId, _this->modes.txt)) {
            _this->saveSettings(); _this->restartDecoder();
        }

        // ---- snap interval ----
        ImGui::LeftLabel("Snap");
        ImGui::FillWidth();
        if (ImGui::Combo(CONCAT("##dsd_snap_", _this->name), &_this->snapId, _this->snapIntervals.txt)) {
            if (_this->vfo) { _this->vfo->setSnapInterval(_this->snapIntervals.value(_this->snapId)); }
            _this->saveSettings();
        }

        // ---- VFO bandwidth (live DSP, no restart) ----
        ImGui::LeftLabel("Bandwidth");
        ImGui::FillWidth();
        if (ImGui::SliderFloat(CONCAT("##dsd_bw_", _this->name), &_this->vfoBandwidth,
                               6250.0f, 25000.0f, "%.0f Hz")) {
            if (_this->vfo) { _this->vfo->setBandwidth(_this->vfoBandwidth); }
            if (_this->enabled && _this->fm) { _this->fm->setBandwidth(_this->vfoBandwidth); }
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) { _this->saveSettings(); }

        // ---- input gain (live DSP, no restart) ----
        ImGui::LeftLabel("Input gain");
        ImGui::FillWidth();
        ImGui::SliderFloat(CONCAT("##dsd_gain_", _this->name), &_this->inputGain,
                           0.1f, 8.0f, "%.2fx");
        if (ImGui::IsItemDeactivatedAfterEdit()) { _this->saveSettings(); }

        // ---- polarity inversions (need restart) ----
        if (ImGui::Checkbox(CONCAT("Invert DMR (-xr)##dsd_xr_", _this->name), &_this->invertDMR)) {
            _this->saveSettings(); _this->restartDecoder();
        }
        ImGui::SameLine();
        if (ImGui::Checkbox(CONCAT("Invert dPMR (-xd)##dsd_xd_", _this->name), &_this->invertDPMR)) {
            _this->saveSettings(); _this->restartDecoder();
        }

        // ---- verbose payload log (need restart) ----
        if (ImGui::Checkbox(CONCAT("Verbose payload log (-Z)##dsd_z_", _this->name), &_this->payloadLog)) {
            _this->saveSettings(); _this->restartDecoder();
        }

        // ---- LRRP folder picker (need restart so dsd-fme writes the new file) ----
        ImGui::LeftLabel("LRRP folder");
        if (_this->lrrpFolderSelect && _this->lrrpFolderSelect->render(CONCAT("##dsd_lrrpfolder_", _this->name))) {
            _this->saveSettings(); _this->restartDecoder();
        }
        if (_this->lrrpFolderSelect && _this->lrrpFolderSelect->pathIsValid()) {
            ImGui::TextDisabled("File: dsd_lrrp_%s.txt", _this->name.c_str());
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "Invalid folder");
        }

        // ---- advanced ----
        if (ImGui::CollapsingHeader(CONCAT("Advanced##dsd_adv_", _this->name))) {
            char buf[512];

            ImGui::LeftLabel("dsd-fme path");
            ImGui::FillWidth();
            strncpy(buf, _this->exePath.c_str(), sizeof(buf) - 1); buf[sizeof(buf)-1] = 0;
            if (ImGui::InputText(CONCAT("##dsd_exe_", _this->name), buf, sizeof(buf))) {
                _this->exePath = buf; _this->saveSettings();
            }

            ImGui::LeftLabel("Key (-H hex)");
            ImGui::FillWidth();
            strncpy(buf, _this->encKey.c_str(), sizeof(buf) - 1); buf[sizeof(buf)-1] = 0;
            if (ImGui::InputText(CONCAT("##dsd_key_", _this->name), buf, sizeof(buf))) {
                _this->encKey = buf; _this->saveSettings();
            }

            ImGui::LeftLabel("Extra args");
            ImGui::FillWidth();
            strncpy(buf, _this->extraArgs.c_str(), sizeof(buf) - 1); buf[sizeof(buf)-1] = 0;
            if (ImGui::InputText(CONCAT("##dsd_extra_", _this->name), buf, sizeof(buf))) {
                _this->extraArgs = buf; _this->saveSettings();
            }

            ImGui::TextDisabled("UDP audio port: %u", (unsigned)_this->udpPort);
        }

        // ---- Trunking (None / Control Channel / Voice Channel) -------------
        if (ImGui::CollapsingHeader(CONCAT("Trunking##dsd_trk_hdr_", _this->name))) {

            // ---- Channel type combo (drives the whole sub-UI below) -------
            const char* const TYPE_LABELS[] = { "None", "Control Channel (CC)", "Voice Channel (VC)" };
            int curType = (int)_this->channelType;
            ImGui::LeftLabel("Channel type");
            ImGui::FillWidth();
            if (ImGui::Combo(CONCAT("##dsd_chtype_", _this->name), &curType,
                             TYPE_LABELS, IM_ARRAYSIZE(TYPE_LABELS))) {
                _this->applyChannelType((ChannelType)curType);
                _this->saveSettings();
            }

            // ---- Big colored status banner (state-aware) ------------------
            ImVec2 region = ImGui::GetContentRegionAvail();
            ImVec2 cur    = ImGui::GetCursorScreenPos();
            const float bannerH = 56.0f;
            ImDrawList* dl = ImGui::GetWindowDrawList();

            ImU32 bg, fg, accent;
            const char* label    = "INACTIVE";
            char freqTxt[64]     = "—";
            char detailTxt[200]  = "";

            switch (_this->channelType) {
                case ChannelType::CC: {
                    bg     = IM_COL32(20, 110, 30, 255);
                    fg     = IM_COL32(180, 255, 180, 255);
                    accent = IM_COL32(80, 200, 100, 255);
                    label  = "CONTROL CHANNEL (server)";
                    snprintf(freqTxt, sizeof(freqTxt), "%.6f MHz", _this->ccFreq / 1e6);
                    int nCli = _this->rigServer.isRunning() ? _this->rigServer.passiveClientCount() : 0;
                    snprintf(detailTxt, sizeof(detailTxt),
                             "%llu grants relayed   %d VC client(s) connected",
                             (unsigned long long)_this->grantCount.load(), nCli);
                    break;
                }
                case ChannelType::VC: {
                    bool conn = _this->rigClient.isConnected();
                    bool hasGrant = (_this->currentVCFreq > 0.0);
                    if (conn && hasGrant) {
                        bg     = IM_COL32(150, 80, 10, 255);
                        fg     = IM_COL32(255, 220, 160, 255);
                        accent = IM_COL32(255, 160, 50, 255);
                        label  = "VOICE CHANNEL (following)";
                    } else if (conn) {
                        bg     = IM_COL32(20, 90, 110, 255);
                        fg     = IM_COL32(180, 230, 255, 255);
                        accent = IM_COL32(80, 170, 220, 255);
                        label  = "VOICE CHANNEL (idle, waiting for grant)";
                    } else {
                        bg     = IM_COL32(110, 50, 20, 255);
                        fg     = IM_COL32(255, 200, 180, 255);
                        accent = IM_COL32(220, 110, 60, 255);
                        label  = "VOICE CHANNEL (disconnected, retrying)";
                    }
                    if (hasGrant) {
                        snprintf(freqTxt, sizeof(freqTxt), "%.6f MHz", _this->currentVCFreq / 1e6);
                        std::time_t now = std::time(nullptr);
                        int dur = (int)(now - _this->vcStartTime);
                        snprintf(detailTxt, sizeof(detailTxt),
                                 "Following CC server at %s:%d   %02d:%02d",
                                 _this->vcServerHost.c_str(), _this->vcServerPort,
                                 dur / 60, dur % 60);
                    } else {
                        snprintf(detailTxt, sizeof(detailTxt),
                                 "Connecting to %s:%d   (no grant yet)",
                                 _this->vcServerHost.c_str(), _this->vcServerPort);
                    }
                    break;
                }
                default: {
                    bg     = IM_COL32(60, 60, 60, 255);
                    fg     = IM_COL32(200, 200, 200, 255);
                    accent = IM_COL32(120, 120, 120, 255);
                    label  = "TRUNKING DISABLED";
                    snprintf(detailTxt, sizeof(detailTxt),
                             "Pick a Channel type above to enable CC or VC mode.");
                    break;
                }
            }

            dl->AddRectFilled(cur, ImVec2(cur.x + region.x, cur.y + bannerH), bg, 6.0f);
            dl->AddRectFilled(cur, ImVec2(cur.x + 6, cur.y + bannerH), accent, 6.0f);
            dl->AddText(ImVec2(cur.x + 14, cur.y + 6),  fg, label);
            dl->AddText(ImVec2(cur.x + 14, cur.y + 22), IM_COL32_WHITE, freqTxt);
            dl->AddText(ImVec2(cur.x + 14, cur.y + 38), fg, detailTxt);
            ImGui::Dummy(ImVec2(0, bannerH + 4));

            // ============== Sub-view: CONTROL CHANNEL ======================
            if (_this->channelType == ChannelType::CC) {
                ImGui::LeftLabel("Server port");
                ImGui::FillWidth();
                int port = _this->rigctlPort;
                if (ImGui::InputInt(CONCAT("##dsd_rport_", _this->name), &port, 1, 100)) {
                    if (port >= 1 && port <= 65535) { _this->rigctlPort = port; }
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    _this->saveSettings();
                    _this->stopRigServer();
                    _this->startRigServer();
                    _this->restartDecoder();
                }

                if (_this->rigServer.isRunning()) {
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
                                       "Server: listening on 127.0.0.1:%d — %d VC client(s)",
                                       _this->rigServer.getPort(),
                                       _this->rigServer.passiveClientCount());
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f),
                                       "Server: failed to listen (port in use?)");
                }

                ImGui::LeftLabel("CC freq (MHz)");
                ImGui::FillWidth();
                double ccMHz = _this->ccFreq / 1e6;
                if (ImGui::InputDouble(CONCAT("##dsd_ccfreq_", _this->name), &ccMHz, 0.0, 0.0, "%.6f")) {
                    _this->ccFreq = ccMHz * 1e6;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) { _this->saveSettings(); }

                if (ImGui::Button(CONCAT("Capture current VFO as CC##dsd_cap_", _this->name))) {
                    _this->ccFreq = _this->getCurrentVfoFreq();
                    _this->saveSettings();
                }

                char buf2[1024];
                ImGui::LeftLabel("Channel map (-C)");
                ImGui::FillWidth();
                strncpy(buf2, _this->trunkChanMap.c_str(), sizeof(buf2) - 1); buf2[sizeof(buf2)-1] = 0;
                if (ImGui::InputTextWithHint(CONCAT("##dsd_cmap_", _this->name),
                                             "path/to/channel_map.csv", buf2, sizeof(buf2))) {
                    _this->trunkChanMap = buf2; _this->saveSettings();
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) { _this->restartDecoder(); }

                ImGui::LeftLabel("Groups (-G)");
                ImGui::FillWidth();
                strncpy(buf2, _this->trunkGroups.c_str(), sizeof(buf2) - 1); buf2[sizeof(buf2)-1] = 0;
                if (ImGui::InputTextWithHint(CONCAT("##dsd_grp_", _this->name),
                                             "path/to/groups.csv", buf2, sizeof(buf2))) {
                    _this->trunkGroups = buf2; _this->saveSettings();
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) { _this->restartDecoder(); }
            }

            // ============== Sub-view: VOICE CHANNEL ========================
            if (_this->channelType == ChannelType::VC) {
                char hbuf[256];
                ImGui::LeftLabel("CC host");
                ImGui::FillWidth();
                strncpy(hbuf, _this->vcServerHost.c_str(), sizeof(hbuf) - 1); hbuf[sizeof(hbuf)-1] = 0;
                if (ImGui::InputText(CONCAT("##dsd_vchost_", _this->name), hbuf, sizeof(hbuf))) {
                    _this->vcServerHost = hbuf;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    _this->saveSettings();
                    _this->rigClient.configure(_this->vcServerHost, _this->vcServerPort);
                }

                ImGui::LeftLabel("CC port");
                ImGui::FillWidth();
                int vp = _this->vcServerPort;
                if (ImGui::InputInt(CONCAT("##dsd_vcport_", _this->name), &vp, 1, 100)) {
                    if (vp >= 1 && vp <= 65535) { _this->vcServerPort = vp; }
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    _this->saveSettings();
                    _this->rigClient.configure(_this->vcServerHost, _this->vcServerPort);
                }

                if (_this->rigClient.isConnected()) {
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
                                       "Client: connected to %s:%d",
                                       _this->vcServerHost.c_str(), _this->vcServerPort);
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                                       "Client: not connected (retrying every 3s)");
                }
            }
        }

        // ---- TCP map output (LRRP positions, type:"lrrp") ------------------
        // Same layout as the ADS-B module: a "TCP map output" collapsing
        // header, Host/Port fields with left labels, and the Enable toggle
        // at the bottom with a compact status indicator on its right.
        if (ImGui::CollapsingHeader(CONCAT("TCP map output##dsd_map_hdr_", _this->name))) {
            char hbuf[256];
            ImGui::LeftLabel("Host");
            ImGui::FillWidth();
            strncpy(hbuf, _this->tcpHost.c_str(), sizeof(hbuf) - 1); hbuf[sizeof(hbuf)-1] = 0;
            if (ImGui::InputText(CONCAT("##dsd_tcp_host_", _this->name), hbuf, sizeof(hbuf))) {
                _this->tcpHost = hbuf;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                _this->saveSettings();
                _this->tcpSender.configure(_this->tcpHost, _this->tcpPort);
            }

            ImGui::LeftLabel("Port");
            ImGui::FillWidth();
            int tp = _this->tcpPort;
            if (ImGui::InputInt(CONCAT("##dsd_tcp_port_", _this->name), &tp, 1, 100)) {
                if (tp >= 1 && tp <= 65535) { _this->tcpPort = tp; }
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                _this->saveSettings();
                _this->tcpSender.configure(_this->tcpHost, _this->tcpPort);
            }

            if (ImGui::Checkbox(CONCAT("Enable TCP##dsd_tcp_en_", _this->name),
                                &_this->tcpEnabled)) {
                _this->tcpSender.setEnabled(_this->tcpEnabled);
                _this->saveSettings();
            }
            ImGui::SameLine();
            if (!_this->tcpEnabled) {
                ImGui::TextDisabled("disabled");
            } else if (_this->tcpSender.isConnected()) {
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "connected");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "connecting...");
            }
        }

        ImGui::Separator();

        // ---- status ----
        bool alive = _this->proc && _this->proc->isRunning() && _this->decoderRunning;
        if (alive) {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Decoder: running");
        } else if (_this->enabled) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Decoder: stopped");
        } else {
            ImGui::TextDisabled("Decoder: module disabled");
        }
        {
            std::lock_guard<std::mutex> lck(_this->consoleMtx);
            ImGui::Text("Lines: %zu", _this->consoleLines.size());
        }
        if (_this->droppedSamples > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                               "Dropped samples: %llu",
                               (unsigned long long)_this->droppedSamples.load());
        }

        // ---- controls ----
        if (ImGui::Button(CONCAT("Apply & (re)start##dsd_restart_", _this->name),
                          ImVec2(menuWidth, 0))) {
            _this->restartDecoder();
        }
        if (ImGui::Button(CONCAT("Open DSD-FME console##dsd_openwin_", _this->name),
                          ImVec2(menuWidth, 0))) {
            _this->showWindow = true;
        }

        if (!_this->enabled) { style::endDisabled(); }

        if (_this->showWindow) { _this->drawWindow(); }
    }

    void drawWindow() {
        std::string title = "DSD-FME — " + name + "###dsd_win_" + name;
        ImGui::SetNextWindowSize(ImVec2(820, 460), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(title.c_str(), &showWindow)) {
            ImGui::End();
            return;
        }

        // Prevent the waterfall from reacting to clicks/drags inside this
        // window: SDR++'s waterfall handler uses raw mouse state plus a
        // geometric hit test that ignores overlapping ImGui windows.
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                                   ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
            || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            gui::mainWindow.lockWaterfallControls = true;
        }

        // ---- toolbar ----
        if (ImGui::Button(CONCAT("Clear##dsd_clear_", name))) {
            { std::lock_guard<std::mutex> l(consoleMtx); consoleLines.clear(); }
            { std::lock_guard<std::mutex> l(lrrpMtx);    lrrp.clear(); }
            { std::lock_guard<std::mutex> l(callsMtx);   calls.clear(); }
        }
        ImGui::SameLine();
        if (ImGui::Button(CONCAT("Save log##dsd_save_", name))) { saveLog(); }
        ImGui::SameLine();
        if (ImGui::Button(CONCAT("Save calls CSV##dsd_savecalls_", name))) { saveCallLogCsv(); }
        ImGui::SameLine();
        ImGui::Checkbox(CONCAT("Auto-scroll##dsd_as_", name), &autoScroll);
        ImGui::SameLine();
        ImGui::Checkbox(CONCAT("Pause##dsd_pause_", name), &paused);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputTextWithHint(CONCAT("##dsd_filter_", name), "filter…",
                                 filterBuf, sizeof(filterBuf));

        if (ImGui::BeginTabBar(CONCAT("##dsd_tabs_", name))) {
            // ---- console ----
            if (ImGui::BeginTabItem(CONCAT("Console##dsd_tab_con_", name))) {
                ImGui::BeginChild(CONCAT("##dsd_con_child_", name), ImVec2(0, 0), true,
                                  ImGuiWindowFlags_HorizontalScrollbar);
                std::string filt = filterBuf;
                std::transform(filt.begin(), filt.end(), filt.begin(), ::tolower);
                {
                    std::lock_guard<std::mutex> lck(consoleMtx);
                    for (const auto& cl : consoleLines) {
                        if (!filt.empty()) {
                            std::string low = cl.text;
                            std::transform(low.begin(), low.end(), low.begin(), ::tolower);
                            if (low.find(filt) == std::string::npos) { continue; }
                        }
                        ImVec4 col = lineColor(cl.text);
                        ImGui::TextColored(col, "%s", cl.text.c_str());
                    }
                }
                if (autoScroll && !paused && consoleDirty &&
                    ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) {
                    ImGui::SetScrollHereY(1.0f);
                }
                consoleDirty = false;
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            // ---- Calls (parsed call history) ----
            if (ImGui::BeginTabItem(CONCAT("Calls##dsd_tab_calls_", name))) {
                // Close stale active calls before rendering.
                sweepStaleCalls();

                const ImGuiTableFlags tf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                           ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                                           ImGuiTableFlags_SizingStretchProp |
                                           ImGuiTableFlags_Sortable;
                std::string filt = filterBuf;
                std::transform(filt.begin(), filt.end(), filt.begin(), ::tolower);

                if (ImGui::BeginTable(CONCAT("##dsd_calls_tbl_", name), 8, tf)) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Time",     ImGuiTableColumnFlags_WidthFixed,    70.0f);
                    ImGui::TableSetupColumn("Dur (s)",  ImGuiTableColumnFlags_WidthFixed,    60.0f);
                    ImGui::TableSetupColumn("Proto",    ImGuiTableColumnFlags_WidthFixed,    72.0f);
                    ImGui::TableSetupColumn("Slot",     ImGuiTableColumnFlags_WidthFixed,    44.0f);
                    ImGui::TableSetupColumn("TG",       ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("ID (src)", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("CC/NAC",   ImGuiTableColumnFlags_WidthFixed,    72.0f);
                    ImGui::TableSetupColumn("Enc",      ImGuiTableColumnFlags_WidthFixed,    40.0f);
                    ImGui::TableHeadersRow();

                    std::time_t now = std::time(nullptr);
                    std::lock_guard<std::mutex> lck(callsMtx);
                    // Newest first: iterate in reverse.
                    for (auto it = calls.rbegin(); it != calls.rend(); ++it) {
                        const CallEntry& c = *it;

                        if (!filt.empty()) {
                            std::string blob = c.tg + " " + c.src + " " + c.protocol +
                                               " " + c.cc + " " + c.nac;
                            std::transform(blob.begin(), blob.end(), blob.begin(), ::tolower);
                            if (blob.find(filt) == std::string::npos) { continue; }
                        }

                        char tbuf[16];
                        std::tm tmv;
#ifdef _WIN32
                        localtime_s(&tmv, &c.startTs);
#else
                        localtime_r(&c.startTs, &tmv);
#endif
                        std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmv);

                        std::time_t et = c.closed ? c.endTs : now;
                        int dur = (int)(et - c.startTs);

                        ImGui::TableNextRow();

                        // Green tint for active calls.
                        if (!c.closed) {
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                ImGui::GetColorU32(ImVec4(0.10f, 0.30f, 0.10f, 0.40f)));
                        }

                        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(tbuf);
                        ImGui::TableSetColumnIndex(1); ImGui::Text("%d", dur);
                        ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(c.protocol.c_str());
                        ImGui::TableSetColumnIndex(3);
                        if (c.slot) { ImGui::Text("%d", c.slot); }
                        else        { ImGui::TextDisabled("-"); }
                        ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(c.tg.c_str());
                        ImGui::TableSetColumnIndex(5); ImGui::TextUnformatted(c.src.c_str());
                        ImGui::TableSetColumnIndex(6);
                        if (!c.cc.empty())       { ImGui::Text("CC=%s",  c.cc.c_str()); }
                        else if (!c.nac.empty()) { ImGui::Text("NAC=%s", c.nac.c_str()); }
                        ImGui::TableSetColumnIndex(7);
                        if (c.enc) {
                            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "ENC");
                        } else {
                            ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "clr");
                        }
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }

            // ---- LRRP / GPS ----
            if (ImGui::BeginTabItem(CONCAT("LRRP / GPS##dsd_tab_lrrp_", name))) {
                const ImGuiTableFlags tf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                           ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                                           ImGuiTableFlags_SizingStretchProp;
                if (ImGui::BeginTable(CONCAT("##dsd_lrrp_tbl_", name), 7, tf)) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Time",     ImGuiTableColumnFlags_WidthFixed, 70.0f);
                    ImGui::TableSetupColumn("Source",   ImGuiTableColumnFlags_WidthFixed, 90.0f);
                    ImGui::TableSetupColumn("Lat",      ImGuiTableColumnFlags_WidthFixed, 90.0f);
                    ImGui::TableSetupColumn("Lon",      ImGuiTableColumnFlags_WidthFixed, 90.0f);
                    ImGui::TableSetupColumn("Spd km/h", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                    ImGui::TableSetupColumn("Dir",      ImGuiTableColumnFlags_WidthFixed, 50.0f);
                    ImGui::TableSetupColumn("Raw",      ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();
                    {
                        std::lock_guard<std::mutex> lck(lrrpMtx);
                        for (const auto& e : lrrp) {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(e.time.c_str());
                            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(e.source.c_str());
                            ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(e.lat.c_str());
                            ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(e.lon.c_str());
                            ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(e.speed.c_str());
                            ImGui::TableSetColumnIndex(5); ImGui::TextUnformatted(e.dir.c_str());
                            ImGui::TableSetColumnIndex(6); ImGui::TextUnformatted(e.raw.c_str());
                        }
                    }
                    if (autoScroll && !paused) { ImGui::SetScrollHereY(1.0f); }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::End();
    }

    static ImVec4 lineColor(const std::string& s) {
        auto has = [&](const char* k) { return s.find(k) != std::string::npos; };
        if (has("ENC") || has("Encrypt") || has("encrypt")) { return ImVec4(1.0f, 0.45f, 0.45f, 1.0f); }
        if (has("VOICE") || has("Voice"))                   { return ImVec4(0.55f, 0.9f, 1.0f, 1.0f); }
        if (has("LRRP") || has("Lat"))                      { return ImVec4(0.6f, 1.0f, 0.6f, 1.0f); }
        if (has("Sync") || has("sync"))                     { return ImVec4(1.0f, 0.85f, 0.4f, 1.0f); }
        return ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
    }

    void saveLog() {
        std::string path = core::args["root"].s() + "/dsd_log_" + name + ".txt";
        std::ofstream f(path, std::ios::trunc);
        if (!f) { return; }
        std::lock_guard<std::mutex> lck(consoleMtx);
        for (const auto& cl : consoleLines) { f << cl.text << "\n"; }
        flog::info("dsd_decoder: saved log to {}", path);
    }

    // ================= members ======================================

    std::string name;
    std::atomic<bool> enabled{true}; // matches construction state (radio-module convention)

    // VFO + input DSP (rebuilt on every enable/disable cycle).
    VFOManager::VFO* vfo = nullptr;
    std::unique_ptr<dsp::demod::FM<float>>      fm;
    std::unique_ptr<dsp::sink::Handler<float>>  audioSink;

    // Output audio stream (registered once with sinkManager).
    dsp::stream<dsp::stereo_t> outAudio;
    SinkManager::Stream        outAudioStream;
    EventHandler<float>        srChangeHandler;
    float                      currentSinkRate = SDRPP_OUT_SR;

    // Subprocess + IO threads.
    std::unique_ptr<DsdProcess> proc;
    std::atomic<bool> decoderRunning{false};
    std::thread writerThread;
    std::thread udpThread;
    std::thread lrrpThread;
    int      udpSock = -1;
    uint16_t udpPort = 0;

    // PCM ring for INPUT audio to dsd-fme.
    std::deque<int16_t> pcmQueue;
    std::mutex pcmMtx;
    std::condition_variable pcmCv;
    std::atomic<uint64_t> droppedSamples{0};

    // Console buffer.
    std::deque<ConsoleLine> consoleLines;
    std::mutex consoleMtx;
    bool consoleDirty = false;

    // LRRP buffer.
    std::deque<LrrpEntry> lrrp;
    std::mutex lrrpMtx;
    std::streamoff lrrpOffset = 0;

    // Call history buffer.
    std::deque<CallEntry> calls;
    std::mutex            callsMtx;
    std::string           lastSeenProto;
    std::string           lastSeenCC;

    // Window state.
    bool showWindow = false;
    bool autoScroll = true;
    bool paused = false;
    char filterBuf[128] = { 0 };

    // Settings.
    OptionList<std::string, std::string> modes;
    OptionList<int, double>              snapIntervals;
    int   modeId       = 0;
    int   snapId       = 1;     // default = 1 kHz
    float vfoBandwidth = 12500.0f;
    float inputGain    = 1.0f;
    bool  invertDMR    = false;
    bool  invertDPMR   = false;
    bool  payloadLog   = false;
    std::string encKey;
    std::string extraArgs;
    std::string exePath = "dsd-fme";
    std::unique_ptr<FolderSelect> lrrpFolderSelect;

    // Trunking. The module operates in one of three roles:
    //   None : no trunking at all.
    //   CC   : control-channel decoder. dsd-fme is invoked with -T -U so
    //          grants come through our embedded rigctl server. We DON'T
    //          retune our VFO on grants; instead we BROADCAST the F command
    //          to every connected VC instance.
    //   VC   : voice-channel follower. dsd-fme is invoked WITHOUT -T -U
    //          (it just decodes audio). A rigctl client thread connects to
    //          the CC instance and retunes our VFO on every F received.
    ChannelType channelType = ChannelType::None;
    int         rigctlPort  = 4532;   // CC mode: listen port
    std::string vcServerHost = "127.0.0.1"; // VC mode: CC peer
    int         vcServerPort = 4532;
    std::string trunkChanMap;
    std::string trunkGroups;

    // Internal RIGCTL server + client + shared CC/VC state.
    RigctlInternalServer  rigServer;   // used in CC mode
    RigctlClient          rigClient;   // used in VC mode
    std::atomic<RigState> rigState{RigState::Inactive};
    double                ccFreq         = 0.0;
    double                currentVCFreq  = 0.0;
    std::time_t           vcStartTime    = 0;
    std::time_t           lastVoiceActivityTs = 0;
    std::atomic<uint64_t> grantCount{0};
    std::deque<GrantEvent> grantHistory;
    std::mutex             grantMtx;
    bool                  autoReturnEnabled = true;
    float                 autoReturnSec     = 3.0f;
    static constexpr size_t GRANT_HISTORY_MAX = 200;

    // LRRP -> Django map server (TCP, JSON lines, type:"lrrp").
    TcpLineSender tcpSender;
    bool          tcpEnabled = false;
    std::string   tcpHost = "127.0.0.1";
    int           tcpPort = 10100;
};

MOD_EXPORT void _INIT_() {
    config.setPath(core::args["root"].s() + "/dsd_decoder_config.json");
    config.load(json::object());
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new DSDDecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (DSDDecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
