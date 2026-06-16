/*
 * Digital Radio Mondiale (DRM) decoder module for SDR++.
 *
 * The module captures baseband I/Q from an SDR++ VFO, feeds it to the Dream
 * DRM receiver core (https://github.com/rafael2k/dream, GPL-2) through a
 * lock-free ring buffer, and routes the decoded audio back into the SDR++
 * sink manager. The full DRM demodulation chain (OFDM synchronisation,
 * channel estimation, MLC/Viterbi decoding, FAC/SDC parsing, MSC
 * de-multiplexing and AAC/xHE-AAC/Opus audio decoding) is handled by Dream.
 */

#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/widgets/constellation_diagram.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <dsp/stream.h>
#include <dsp/types.h>
#include <dsp/sink/handler_sink.h>

#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <chrono>

#include "spsc_ring.h"
#include "drm_input.h"
#include "drm_output.h"

/* Dream receiver core */
#include "GlobalDefinitions.h"
#include "Parameter.h"
#include "DrmReceiver.h"
#include "creceivedata.h"
#include "util/Settings.h"
#include "SDC/audioparam.h"

SDRPP_MOD_INFO{
    /* Name:            */ "drm_decoder",
    /* Description:     */ "Digital Radio Mondiale (DRM) decoder for SDR++",
    /* Author:          */ "SDR++ community",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

ConfigManager config;

/* DRM occupies up to 20 kHz; we run the VFO at 48 kHz and feed Dream I/Q. */
#define INPUT_SAMPLE_RATE 48000.0
#define DEFAULT_AUDIO_SR  48000.0

#define CONCAT(a, b) ((std::string(a) + b).c_str())

struct BwOption { const char* label; double bw; };
static const BwOption BW_OPTIONS[] = {
    { "4.5 kHz",        4500.0  },
    { "5 kHz",          5000.0  },
    { "9 kHz",          9000.0  },
    { "10 kHz",         10000.0 },
    { "18 kHz",         18000.0 },
    { "20 kHz",         20000.0 },
};
static const int NUM_BW_OPTIONS = sizeof(BW_OPTIONS) / sizeof(BW_OPTIONS[0]);

/* GUI-facing snapshot of the receiver state, refreshed by the worker thread. */
struct ServiceInfo {
    std::string label;
    std::string codec;
    int         bitrateHint = 0; /* sample rate in Hz, used as a quick hint  */
    bool        isAudio = true;
    uint32_t    id = 0;
};

struct RxSnapshot {
    bool        signalLocked = false;
    double      snr = 0.0;
    char        robustness = '-';
    double      occupancyBw = 0.0;
    int         currentService = 0;
    int         audioSampleRate = 0;
    double      wmerMsc = 0.0;      /* Weighted MER of MSC, more predictive than SNR */
    std::string stationLabel;
    std::string textMessage;
    std::vector<ServiceInfo> services;
};

class DRMDecoderModule : public ModuleManager::Instance {
public:
    DRMDecoderModule(std::string name) {
        this->name = name;

        /* Load persisted settings */
        config.acquire();
        if (!config.conf.contains(name)) {
            config.conf[name]["bwIndex"] = 3; /* 10 kHz */
            config.conf[name]["flipSpectrum"] = false;
            config.conf[name]["mlcIterations"] = 1;
        }
        if (config.conf[name].contains("bwIndex")) {
            bwIndex = config.conf[name]["bwIndex"];
        }
        if (config.conf[name].contains("flipSpectrum")) {
            flipSpectrum = config.conf[name]["flipSpectrum"];
        }
        if (config.conf[name].contains("mlcIterations")) {
            mlcIterations = config.conf[name]["mlcIterations"];
        }
        bwIndex = std::clamp(bwIndex, 0, NUM_BW_OPTIONS - 1);
        mlcIterations = std::clamp((int)mlcIterations.load(), 0, 4);
        config.release(true);

        /* Build the sound bridges (shared with the worker thread) */
        soundIn  = new DRMSoundIn(&ring, &running);
        soundOut = new DRMSoundOut(
            [this](const short* s, int frames) { onAudioSamples(s, frames); },
            [this](int rate) { onAudioSampleRate(rate); });

        /* Audio output stream into the SDR++ sink manager. Start it here (and
           stop it only in the destructor), matching the reference decoders, so
           the SinkManager keeps an audio device bound to this stream across
           enable/disable cycles. */
        srChangeHandler.ctx = this;
        srChangeHandler.handler = sampleRateChangeHandler;
        audioStream.init(&audioIn, &srChangeHandler, DEFAULT_AUDIO_SR);
        sigpath::sinkManager.registerStream(name, &audioStream);
        audioStream.start();

        /* Init the capture sink once on a placeholder stream; the real VFO
           output is attached in startReceiver() and detached in stopReceiver(). */
        iqSink.init(&dummyIq, iqHandler, this);

        gui::menu.registerEntry(name, menuHandler, this, this);

        /* SDR++'s ModuleManager calls the constructor at startup and then
           only calls disable() if the saved config has enabled=false.
           enable() is NOT called automatically - only when the user toggles
           the module on. So we must bring the receiver online here. */
        startReceiver();
        enabled = true;
    }

    ~DRMDecoderModule() {
        if (enabled) { disable(); }
        audioStream.stop();
        sigpath::sinkManager.unregisterStream(name);
        gui::menu.removeEntry(name);
        delete soundIn;
        delete soundOut;
    }

    void postInit() {}

    void enable() {
        startReceiver();
        enabled = true;
    }

    void disable() {
        stopReceiver();
        enabled = false;
    }

    bool isEnabled() { return enabled; }

private:
    /* Bring the receiver online: create the VFO, wire up the I/Q sink, start
       the worker thread. SDR++ calls the constructor at startup (and
       disable() afterwards only if the module was saved as disabled), so this
       work cannot live exclusively in enable() - it must happen in the
       constructor too. enable() is reached only on user toggle. */
    void startReceiver() {
        if (vfo) { return; } /* already running */
        double bw = BW_OPTIONS[bwIndex].bw;
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER,
                                            0, bw, INPUT_SAMPLE_RATE, bw, bw, true);
        if (!vfo) { return; }
        vfo->setSnapInterval(1000);

        iqSink.setInput(vfo->output);

        ring.clear();
        running.store(true, std::memory_order_release);

        iqSink.start();
        worker = std::thread(&DRMDecoderModule::workerLoop, this);
    }

    /* Tear the receiver down: stop worker, release the VFO. Used by both
       disable() and the destructor. Safe to call when nothing is running. */
    void stopReceiver() {
        if (!vfo && !worker.joinable()) { return; }

        /* Tell the worker to stop. Abort the ring first so that if the worker
           is mid-Read and the DSP thread is mid-write(), neither blocks: the
           lossless ring applies backpressure during normal operation, but on
           shutdown we must release any waiter. */
        running.store(false, std::memory_order_release);
        ring.abort();

        /* Stop the DSP feed before joining so the producer (iqHandler on the
           DSP thread) stops pushing into the ring. */
        iqSink.stop();

        if (worker.joinable()) { worker.join(); }

        if (vfo) {
            /* Detach from the VFO output before destroying it */
            iqSink.setInput(&dummyIq);
            sigpath::vfoManager.deleteVFO(vfo);
            vfo = NULL;
        }
    }

    /* ---- DSP: VFO I/Q -> ring (producer, runs on SDR++ DSP thread) ---- */
    static void iqHandler(dsp::complex_t* data, int count, void* ctx) {
        DRMDecoderModule* _this = (DRMDecoderModule*)ctx;
        std::vector<short>& tmp = _this->iqScratch;
        if ((int)tmp.size() < count * 2) { tmp.resize(count * 2); }
        for (int i = 0; i < count; i++) {
            float fi = data[i].re * 32767.0f;
            float fq = data[i].im * 32767.0f;
            if (fi >  32767.0f) fi =  32767.0f; else if (fi < -32768.0f) fi = -32768.0f;
            if (fq >  32767.0f) fq =  32767.0f; else if (fq < -32768.0f) fq = -32768.0f;
            tmp[2 * i]     = (short)lrintf(fi);
            tmp[2 * i + 1] = (short)lrintf(fq);
        }
        _this->ring.write(tmp.data(), (size_t)count * 2);
    }

    /* ---- Audio: Dream -> SDR++ sink (runs on the worker thread) ---- */
    void onAudioSamples(const short* interleaved, int frames) {
        if (!running.load(std::memory_order_acquire)) { return; }
        int off = 0;
        while (off < frames) {
            int n = std::min(frames - off, 16384);
            for (int i = 0; i < n; i++) {
                audioIn.writeBuf[i].l = interleaved[2 * (off + i)]     / 32768.0f;
                audioIn.writeBuf[i].r = interleaved[2 * (off + i) + 1] / 32768.0f;
            }
            if (!audioIn.swap(n)) { break; }
            off += n;
        }
    }

    void onAudioSampleRate(int rate) {
        if (rate <= 0) { return; }
        audioStream.setSampleRate((float)rate);
    }

    static void sampleRateChangeHandler(float, void*) {}

    /* ---- Worker thread: drives the Dream receiver ---- */
    void workerLoop() {
        try {
            CSettings settings;                 /* default, never loaded from disk */
            CDRMReceiver rx(&settings);          /* constructed in RM_DRM mode      */

            CReceiveData* rdata = rx.GetReceiveData();
            CWriteData*   wdata = rx.GetWriteData();
            rdata->SetSoundInterface(soundIn);
            /* The SDR++ VFO delivers zero-IF complex baseband (DRM centred on
               DC). CS_IQ_POS_ZERO mixes that up to Dream's 6 kHz virtual IF
               before the Hilbert stage - the correct transform for a DC-centred
               input, and more robust to frequency offset than CS_IQ_POS (which
               assumes the signal is already sitting at the virtual IF). */
            rdata->SetInChanSel(CS_IQ_POS_ZERO);
            rdata->SetFlippedSpectrum(flipSpectrum.load(std::memory_order_acquire));
            wdata->SetSoundInterface(soundOut);

            /* Number of MLC (multi-level coding) decoder iterations. More
               iterations help recover the audio in a fading/low-SNR channel at
               the cost of CPU. Range 0..4. */
            rx.GetMSCMLC()->SetNumIterations(
                std::clamp(mlcIterations.load(std::memory_order_acquire), 0, 4));

            CParameter* params = rx.GetParameters();

            /* CRITICAL: tell Dream the exact rates of the stream we feed it.
               Without this, Dream keeps default scale ratios / signal sample
               rate that do NOT match our 48 kHz I/Q, so it consumes the input
               at the wrong rate. The robust FAC/SDC still decode (lock, mode,
               label, SNR all appear), but the high-rate MSC audio is starved
               and never produces sound. These five calls pin input, audio and
               up/down-scale ratios to our 48 kHz feed, exactly as Dream's own
               LoadSettings would for a 48 kHz sound device. */
            params->SetNewSoundcardSigSampleRate((int)INPUT_SAMPLE_RATE);
            params->SetNewAudSampleRate((int)DEFAULT_AUDIO_SR);
            params->SetNewSigUpscaleRatio(1);
            params->SetNewSigDownscaleRatio(1);
            params->FetchNewSampleRate();

            rx.InitReceiverMode();
            rx.SetInStartMode();

            bool lastFlip = flipSpectrum.load(std::memory_order_acquire);
            int tick = 0;
            while (running.load(std::memory_order_acquire)) {
                rx.updatePosition();
                rx.process();

                /* Apply a pending service selection from the GUI */
                int sel = pendingService.exchange(-1, std::memory_order_acq_rel);
                if (sel >= 0) {
                    params->SetCurSelAudioService(sel);
                }

                /* Hot-apply a spectrum-flip toggle from the GUI */
                bool wantFlip = flipSpectrum.load(std::memory_order_acquire);
                if (wantFlip != lastFlip) {
                    rdata->SetFlippedSpectrum(wantFlip);
                    lastFlip = wantFlip;
                }

                /* Refresh the constellation only when Dream has a stable
                   lock. During acquisition Dream may reinitialise the MLC
                   internals (e.g. switching from a tentative mode to the
                   detected one), which can free and reallocate the cell
                   vectors mid-process; reading GetVectorSpace() at that
                   instant risks touching freed memory.
                   We re-read AcquiState here (briefly under the param lock)
                   so we never race the transition. */
                bool locked;
                params->Lock();
                locked = (params->GetAcquiState() == AS_WITH_SIGNAL);
                params->Unlock();
                if (locked) {
                    try { updateConstellation(&rx); }
                    catch (...) { /* swallow: receiver may be re-initialising */ }
                }

                /* Refresh the GUI snapshot a few times per second */
                if ((++tick % 8) == 0) {
                    snapshotState(params, &rx);
                }
            }
            rx.CloseSoundInterfaces();
        }
        catch (CGenErr& e)      { /* swallow: receiver is shutting down */ }
        catch (std::string& s)  { }
        catch (...)             { }
    }

    /* Push MSC equalised cells into the constellation widget as a rolling
       buffer. The MLC layer rebuilds its internal cell vector once per OFDM
       frame (~2-3 Hz in mode B), so simply copying that vector to the widget
       on every tick would flash a fully-replaced static image at the frame
       rate - visible as a "frozen" or jerky display between updates. Instead:

         1. Detect when the upstream vector actually changes (cheap signature
            on the first few cells) so we ignore ticks that have nothing new.
         2. On each NEW frame, overwrite only a slice of the widget buffer at
            a rotating write cursor.

       That spreads each block's cells across several frames worth of buffer
       so old points fade out as new ones come in and the eye sees a smooth
       sweep rather than a stuttering full-buffer refresh. */
    static constexpr int CONST_BUF       = 1024;   /* widget buffer size */
    static constexpr int CONST_PER_BLOCK = 256;    /* points written per OFDM frame */
    int    constWritePos = 0;
    double constLastSig  = 0.0;

    void updateConstellation(CDRMReceiver* rx) {
        if (!rx || !rx->GetMSCMLC()) { return; }
        CVector<_COMPLEX> cells;
        rx->GetMSCMLC()->GetVectorSpace(cells);
        int n = cells.Size();
        if (n <= 0) { return; }

        /* Cheap signature to detect that this is a NEW frame's data, not the
           same buffer we already consumed. Just a weighted sum of a few cells
           - it does not need to be cryptographic, only monotonically distinct
           between successive frames, which equalised QAM cells always are. */
        double sig = 0.0;
        int sigN = std::min(n, 16);
        for (int i = 0; i < sigN; i++) {
            sig += cells[i].real() + 7.0 * cells[i].imag();
        }
        if (sig == constLastSig) { return; }   /* same block, nothing new */
        constLastSig = sig;

        /* Normalise so most cells land in [-1, 1]: use the average magnitude
           of a representative sample as the scale reference. */
        double mag = 0.0;
        int magN = std::min(n, 64);
        for (int i = 0; i < magN; i++) {
            double r = cells[i].real(), im = cells[i].imag();
            mag += std::sqrt(r * r + im * im);
        }
        mag = (magN > 0) ? mag / magN : 1.0;
        if (mag < 1e-9) { mag = 1.0; }

        dsp::complex_t* buf = constDiagram.acquireBuffer();
        /* Write CONST_PER_BLOCK new points into the rolling buffer. We sample
           the source vector uniformly so a full OFDM block's worth of cells
           is represented even if it has thousands of points. */
        for (int j = 0; j < CONST_PER_BLOCK; j++) {
            int src = (j * n) / CONST_PER_BLOCK;
            if (src >= n) { src = n - 1; }
            int dst = (constWritePos + j) % CONST_BUF;
            buf[dst].re = (float)(cells[src].real() / mag);
            buf[dst].im = (float)(cells[src].imag() / mag);
        }
        constDiagram.releaseBuffer();
        constWritePos = (constWritePos + CONST_PER_BLOCK) % CONST_BUF;
    }

    void snapshotState(CParameter* params, CDRMReceiver* /*rx*/) {
        RxSnapshot snap;

        params->Lock();
        snap.signalLocked    = (params->GetAcquiState() == AS_WITH_SIGNAL);
        snap.snr             = params->GetSNR();
        snap.audioSampleRate = params->GetAudSampleRate();
        snap.wmerMsc         = (double)params->rWMERMSC;

        switch (params->GetWaveMode()) {
            case RM_ROBUSTNESS_MODE_A: snap.robustness = 'A'; break;
            case RM_ROBUSTNESS_MODE_B: snap.robustness = 'B'; break;
            case RM_ROBUSTNESS_MODE_C: snap.robustness = 'C'; break;
            case RM_ROBUSTNESS_MODE_D: snap.robustness = 'D'; break;
            case RM_ROBUSTNESS_MODE_E: snap.robustness = 'E'; break;
            default:                   snap.robustness = '-'; break;
        }

        static const double SO_BW[] = { 4500, 5000, 9000, 10000, 18000, 20000 };
        int so = (int)params->GetSpectrumOccup();
        if (so >= 0 && so < 6) { snap.occupancyBw = SO_BW[so]; }

        snap.currentService = params->GetCurSelAudioService();

        for (size_t i = 0; i < params->Service.size(); i++) {
            const CService& s = params->Service[i];
            if (!s.IsActive()) { continue; }
            ServiceInfo si;
            si.id      = s.iServiceID;
            si.label   = s.strLabel;
            si.isAudio = (s.eAudDataFlag == CService::SF_AUDIO);
            switch (s.AudioParam.eAudioCoding) {
                case CAudioParam::AC_AAC:     si.codec = "AAC";      break;
                case CAudioParam::AC_OPUS:    si.codec = "Opus";     break;
                case CAudioParam::AC_xHE_AAC: si.codec = "xHE-AAC";  break;
                case CAudioParam::AC_MPEGAAC: si.codec = "MPEG-AAC"; break;
                default:                      si.codec = si.isAudio ? "?" : "data"; break;
            }
            if ((int)i == snap.currentService) {
                snap.stationLabel = s.strLabel;
                snap.textMessage  = s.AudioParam.strTextMessage;
            }
            snap.services.push_back(si);
        }
        params->Unlock();

        std::lock_guard<std::mutex> lck(guiMtx);
        gui_snap = std::move(snap);
    }

    /* ---- GUI ---- */
    static void menuHandler(void* ctx) {
        DRMDecoderModule* _this = (DRMDecoderModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) { style::beginDisabled(); }

        /* Channel bandwidth selector */
        ImGui::LeftLabel("Bandwidth");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        std::string preview = BW_OPTIONS[_this->bwIndex].label;
        if (ImGui::BeginCombo(CONCAT("##drm_bw_", _this->name), preview.c_str())) {
            for (int i = 0; i < NUM_BW_OPTIONS; i++) {
                if (ImGui::Selectable(BW_OPTIONS[i].label, i == _this->bwIndex)) {
                    _this->bwIndex = i;
                    config.acquire();
                    config.conf[_this->name]["bwIndex"] = i;
                    config.release(true);
                    if (_this->enabled && _this->vfo) {
                        _this->vfo->setBandwidthLimits(BW_OPTIONS[i].bw, BW_OPTIONS[i].bw, true);
                        _this->vfo->setBandwidth(BW_OPTIONS[i].bw);
                    }
                }
            }
            ImGui::EndCombo();
        }

        /* Spectrum inversion. Many SDR/mixer chains deliver an inverted
           spectrum; DRM will not lock until this matches. */
        bool flip = _this->flipSpectrum.load();
        if (ImGui::Checkbox(CONCAT("Flip spectrum##drm_flip_", _this->name), &flip)) {
            _this->flipSpectrum.store(flip);
            config.acquire();
            config.conf[_this->name]["flipSpectrum"] = flip;
            config.release(true);
        }

        /* MLC decoder iterations: higher = better audio recovery on weak/fading
           signals, more CPU. Applied when the decoder (re)starts. */
        int mlc = _this->mlcIterations.load();
        ImGui::LeftLabel("MLC iterations");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::SliderInt(CONCAT("##drm_mlc_", _this->name), &mlc, 0, 4)) {
            _this->mlcIterations.store(mlc);
            config.acquire();
            config.conf[_this->name]["mlcIterations"] = mlc;
            config.release(true);
        }
        if (_this->enabled) {
            ImGui::TextDisabled("(toggle the module to apply MLC changes)");
        }

        if (!_this->enabled) { style::endDisabled(); }

        ImGui::Separator();

        /* Snapshot copy for rendering */
        RxSnapshot snap;
        {
            std::lock_guard<std::mutex> lck(_this->guiMtx);
            snap = _this->gui_snap;
        }

        /* Sync indicator */
        ImGui::Text("Status:");
        ImGui::SameLine();
        if (_this->enabled && snap.signalLocked) {
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "SIGNAL LOCKED");
        }
        else {
            ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.2f, 1.0f),
                               _this->enabled ? "Acquiring..." : "Disabled");
        }

        ImGui::Text("Mode:");        ImGui::SameLine();
        ImGui::Text("DRM %c", snap.robustness);
        ImGui::Text("Occupancy:");   ImGui::SameLine();
        if (snap.occupancyBw > 0) { ImGui::Text("%.1f kHz", snap.occupancyBw / 1000.0); }
        else { ImGui::Text("-"); }
        ImGui::Text("SNR:");         ImGui::SameLine();
        ImGui::Text("%.1f dB", snap.snr);

        /* Weighted MER of the MSC channel. Above ~18 dB the audio decodes
           cleanly; below ~14 dB it usually doesn't. */
        if (snap.wmerMsc != 0.0) {
            ImGui::Text("MER MSC:"); ImGui::SameLine();
            if (snap.wmerMsc >= 18.0) {
                ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "%.1f dB", snap.wmerMsc);
            } else if (snap.wmerMsc >= 14.0) {
                ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "%.1f dB", snap.wmerMsc);
            } else {
                ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.2f, 1.0f), "%.1f dB", snap.wmerMsc);
            }
        }

        if (snap.audioSampleRate > 0) {
            ImGui::Text("Audio:");   ImGui::SameLine();
            ImGui::Text("%d Hz", snap.audioSampleRate);
        }

        /* MSC equalised constellation. Tight clusters at the QAM points mean
           a clean channel; smeared or rotating clouds mean noise or sync drift. */
        if (_this->enabled && snap.signalLocked) {
            ImGui::Spacing();
            ImGui::TextUnformatted("MSC constellation");
            ImGui::SetNextItemWidth(menuWidth);
            _this->constDiagram.draw();
        }

        ImGui::Separator();

        /* Service list */
        ImGui::Text("Services");
        ImGui::BeginChild(CONCAT("##drm_svc_", _this->name),
                          ImVec2(menuWidth, 90.0f * style::uiScale), true);
        if (snap.services.empty()) {
            ImGui::TextDisabled("(none)");
        }
        for (size_t i = 0; i < snap.services.size(); i++) {
            const ServiceInfo& s = snap.services[i];
            std::string lbl = s.label.empty() ? "(unnamed)" : s.label;
            std::string row = lbl + "  [" + s.codec + "]";
            bool selected = ((int)i == snap.currentService);
            if (ImGui::Selectable(CONCAT(row + "##drm_svc_row_", std::to_string(i)),
                                  selected) && s.isAudio) {
                _this->pendingService.store((int)i, std::memory_order_release);
            }
        }
        ImGui::EndChild();

        /* Station label + text message */
        if (!snap.stationLabel.empty()) {
            ImGui::Text("Station:"); ImGui::SameLine();
            ImGui::TextWrapped("%s", snap.stationLabel.c_str());
        }
        if (!snap.textMessage.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", snap.textMessage.c_str());
        }
    }

    /* ---- Members ---- */
    std::string name;
    bool        enabled = true;
    int         bwIndex = 3;
    std::atomic<bool> flipSpectrum{ false };
    std::atomic<int>  mlcIterations{ 1 };

    VFOManager::VFO* vfo = NULL;

    SpscRing<short>            ring{ 1u << 20 };  /* ~1M shorts ≈ 10 s of I/Q */
    std::vector<short>         iqScratch;
    dsp::stream<dsp::complex_t>        dummyIq;   /* placeholder input for iqSink */
    dsp::sink::Handler<dsp::complex_t> iqSink;

    dsp::stream<dsp::stereo_t> audioIn;
    SinkManager::Stream        audioStream;
    EventHandler<float>        srChangeHandler;
    ImGui::ConstellationDiagram constDiagram;

    DRMSoundIn*  soundIn  = NULL;
    DRMSoundOut* soundOut = NULL;

    std::thread        worker;
    std::atomic<bool>  running{ false };
    std::atomic<int>   pendingService{ -1 };

    std::mutex  guiMtx;
    RxSnapshot  gui_snap;
};

MOD_EXPORT void _INIT_() {
    config.setPath(core::args["root"].s() + "/drm_decoder_config.json");
    config.load(json::object());
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new DRMDecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (DRMDecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
