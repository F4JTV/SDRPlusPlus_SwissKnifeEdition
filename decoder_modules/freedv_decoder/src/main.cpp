/*
 * FreeDV decoder module for SDR++.
 *
 * Decodes FreeDV digital voice from an HF SSB signal:
 *
 *   VFO (complex baseband) -> dsp::demod::SSB<float> (USB, 8 kHz real audio)
 *       -> FreeDVDSP (libcodec2 FreeDV API) -> stereo speech (8 or 16 kHz)
 *       -> RationalResampler -> sink manager (audible decoded voice)
 *
 * The decoder is driven by libcodec2's FreeDV API, so it tracks upstream
 * codec2 exactly. Supported HF SSB modes (all use an 8 kHz modem rate, so a
 * single front-end serves them): 700D, 700E, 700C, 1600, 800XA, and the
 * LPCNet 2020 / 2020B modes when the linked libcodec2 provides them.
 *
 * Live status (sync flag, SNR estimate) and the low-rate text channel are
 * shown in the module menu. There is no network output: like the CW decoder,
 * this is a listen-and-read voice mode.
 */
#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <signal_path/sink.h>
#include <module.h>
#include <dsp/demod/ssb.h>
#include <dsp/multirate/rational_resampler.h>
#include <utils/flog.h>

#include <algorithm>
#include <string>
#include <vector>

#include "freedv_dsp.h"

#define CONCAT(a, b) ((std::string(a) + b).c_str())

// Modem sample rate. Every supported mode uses 8 kHz, so the VFO and SSB
// demodulator run at this rate regardless of the selected mode.
#define FDV_MODEM_SR 8000.0

SDRPP_MOD_INFO{
    /* Name:            */ "freedv_decoder",
    /* Description:     */ "FreeDV digital voice decoder (libcodec2 FreeDV API)",
    /* Author:          */ "",
    /* Version:         */ 1, 0, 0,
    /* Max instances    */ -1
};

ConfigManager config;

class FreeDVDecoderModule : public ModuleManager::Instance {
public:
    FreeDVDecoderModule(std::string name) {
        this->name = name;

        // Build the mode list (id + display name) and a NUL-separated items
        // string for ImGui::Combo.
        int n = 0;
        const fdvmod::ModeInfo* mt = fdvmod::modeTable(&n);
        for (int i = 0; i < n; i++) {
            modeIds.push_back(mt[i].id);
            modeItems += mt[i].name;
            modeItems.push_back('\0');
        }

        // Load configuration.
        config.acquire();
        if (!config.conf.contains(name)) { config.conf[name] = json::object(); }
        loadOrDefault("modeId",    modeId,    FREEDV_MODE_700D);
        loadOrDefault("bandwidth", bandwidth, 2800.0f);
        config.release(true);

        // Resolve the persisted mode id to a combo index (fall back to first).
        modeSel = fdvmod::modeIndex(modeId);
        modeId = modeIds[modeSel];

        // VFO at the modem rate, USB convention (audio 0 Hz at lower band edge).
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_LOWER,
                                            0, bandwidth, FDV_MODEM_SR, 200, FDV_MODEM_SR, false);
        vfo->setSnapInterval(10);

        // USB demodulation to real 8 kHz audio. Slow AGC (values are in
        // 1/samples) so the OFDM signal dynamics are preserved, like a real
        // SSB receiver feeding freedv-gui.
        ssb.init(vfo->output, dsp::demod::SSB<float>::Mode::USB, bandwidth, FDV_MODEM_SR,
                 50.0 / FDV_MODEM_SR, 5.0 / FDV_MODEM_SR);

        // FreeDV decode -> stereo speech at the mode's speech rate.
        decode.init(&ssb.out, modeId);
        double speechSr = (double)decode.getSpeechSampleRate();

        // Resample decoded speech to the sink rate.
        resamp.init(&decode.out, speechSr, audioSampleRate);
        srChangeHandler.ctx = this;
        srChangeHandler.handler = sampleRateChangeHandler;
        stream.init(&resamp.out, &srChangeHandler, audioSampleRate);
        sigpath::sinkManager.registerStream(name, &stream);

        // Start the processing graph.
        ssb.start();
        decode.start();
        resamp.start();
        stream.start();

        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~FreeDVDecoderModule() {
        gui::menu.removeEntry(name);
        stream.stop();
        if (enabled) {
            ssb.stop();
            decode.stop();
            resamp.stop();
            sigpath::vfoManager.deleteVFO(vfo);
        }
        sigpath::sinkManager.unregisterStream(name);
    }

    void postInit() {}

    void enable() {
        double bw = gui::waterfall.getBandwidth();
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_LOWER,
                                            std::clamp<double>(0, -bw / 2.0, bw / 2.0),
                                            bandwidth, FDV_MODEM_SR, 200, FDV_MODEM_SR, false);
        vfo->setSnapInterval(10);
        ssb.setInput(vfo->output);
        ssb.start();
        decode.start();
        resamp.start();
        enabled = true;
    }

    void disable() {
        ssb.stop();
        decode.stop();
        resamp.stop();
        sigpath::vfoManager.deleteVFO(vfo);
        enabled = false;
    }

    bool isEnabled() { return enabled; }

private:
    template <typename T>
    void loadOrDefault(const char* key, T& dst, const T& def) {
        if (config.conf[name].contains(key)) { dst = config.conf[name][key].get<T>(); }
        else { dst = def; config.conf[name][key] = def; }
    }

    void saveConf() {
        config.acquire();
        config.conf[name]["modeId"]    = modeId;
        config.conf[name]["bandwidth"] = bandwidth;
        config.release(true);
    }

    void setAudioSampleRate(double sr) {
        audioSampleRate = sr;
        resamp.setOutSamplerate(audioSampleRate);
    }

    void applyMode(int newModeId) {
        modeId = newModeId;
        double speechSr = (double)decode.setMode(modeId);
        resamp.setInSamplerate(speechSr);
        saveConf();
    }

    static void sampleRateChangeHandler(float sampleRate, void* ctx) {
        FreeDVDecoderModule* _this = (FreeDVDecoderModule*)ctx;
        _this->setAudioSampleRate(sampleRate);
    }

    static void menuHandler(void* ctx) {
        FreeDVDecoderModule* _this = (FreeDVDecoderModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) { style::beginDisabled(); }

        // --- Mode ---
        ImGui::LeftLabel("Mode");
        ImGui::FillWidth();
        if (ImGui::Combo(CONCAT("##fdv_mode_", _this->name), &_this->modeSel,
                         _this->modeItems.c_str())) {
            _this->applyMode(_this->modeIds[_this->modeSel]);
        }

        // --- Channel bandwidth (USB passband fed to the modem) ---
        ImGui::LeftLabel("Filter (Hz)");
        ImGui::FillWidth();
        if (ImGui::SliderFloat(CONCAT("##fdv_bw_", _this->name), &_this->bandwidth,
                               1000.0f, 3000.0f, "%.0f")) {
            if (_this->enabled) { _this->vfo->setBandwidth(_this->bandwidth); }
            _this->ssb.setBandwidth(_this->bandwidth);
            _this->saveConf();
        }

        ImGui::Separator();

        // --- Sync status ---
        bool synced = _this->decode.synced();
        ImGui::Text("Status:");
        ImGui::SameLine();
        if (synced) {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "SYNC");
        } else {
            ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.2f, 1.0f), "no sync");
        }

        // --- SNR estimate ---
        float snr = _this->decode.snr();
        ImGui::Text("SNR: %+.1f dB", snr);
        float bar = std::clamp((snr + 5.0f) / 25.0f, 0.0f, 1.0f); // -5..+20 dB
        ImGui::FillWidth();
        ImGui::ProgressBar(bar, ImVec2(0, 0));

        // --- Text channel ---
        ImGui::Text("Text channel:");
        ImVec2 boxSize(menuWidth, 90.0f * style::uiScale);
        ImGui::BeginChild(CONCAT("##fdv_txt_", _this->name), boxSize, true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        {
            std::string txt = _this->decode.getText();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(txt.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();

        if (ImGui::Button(CONCAT("Clear##fdv_clear_", _this->name), ImVec2(menuWidth, 0))) {
            _this->decode.clearText();
        }

        if (!_this->enabled) { style::endDisabled(); }
    }

    std::string name;
    bool enabled = true;

    VFOManager::VFO* vfo = NULL;

    // DSP chain.
    dsp::demod::SSB<float> ssb;
    fdvmod::FreeDVDSP decode;
    dsp::multirate::RationalResampler<dsp::stereo_t> resamp;
    SinkManager::Stream stream;
    EventHandler<float> srChangeHandler;
    double audioSampleRate = 48000.0;

    // Mode selection.
    std::vector<int> modeIds;
    std::string modeItems;  // NUL-separated for ImGui::Combo
    int modeSel = 0;

    // Persisted settings.
    int modeId = FREEDV_MODE_700D;
    float bandwidth = 2800.0f;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/freedv_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new FreeDVDecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (FreeDVDecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
