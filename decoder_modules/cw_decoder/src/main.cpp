#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <signal_path/sink.h>
#include <module.h>
#include <dsp/sink/handler_sink.h>
#include <dsp/routing/splitter.h>
#include <dsp/demod/cw.h>
#include <dsp/multirate/rational_resampler.h>
#include <utils/flog.h>
#include <algorithm>
#include <string>
#include <mutex>
#include "cw_dsp.h"
#include "cw_decoder.h"

#define CONCAT(a, b) ((std::string(a) + b).c_str())

// Internal processing rate. Kept at 8 kHz so the FLDigi-derived timing
// constants apply unchanged.
#define CW_DSP_SR 8000.0

SDRPP_MOD_INFO{
    /* Name:            */ "cw_decoder",
    /* Description:     */ "CW (Morse) decoder, FLDigi-style adaptive decoding",
    /* Author:          */ "F4JTV",
    /* Version:         */ 1, 0, 0,
    /* Max instances    */ -1
};

ConfigManager config;

class CWDecoderModule : public ModuleManager::Instance {
public:
    CWDecoderModule(std::string name) {
        this->name = name;

        // Load configuration.
        config.acquire();
        if (!config.conf.contains(name)) { config.conf[name] = json::object(); }
        loadOrDefault("speed",        speed,        18);
        loadOrDefault("range",        range,        10);
        loadOrDefault("tracking",     tracking,     true);
        loadOrDefault("bandwidth",    bandwidth,    200.0f);
        loadOrDefault("squelchOn",    squelchOn,    false);
        loadOrDefault("squelchLevel", squelchLevel, 10.0f);
        loadOrDefault("prosigns",     prosigns,     true);
        loadOrDefault("tone",         tone,         600);
        config.release(true);

        // Decoder configuration and text output.
        decoder.setSpeed(speed);
        decoder.setRange(range);
        decoder.setTracking(tracking);
        decoder.setSquelch(squelchOn, squelchLevel);
        decoder.setProsignDisplay(prosigns);
        decoder.onChar = [this](const std::string& s) { onChar(s); };

        // Create the VFO (8 kHz channel, adjustable width).
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0,
                                            bandwidth, CW_DSP_SR, 50, 1000, false);
        vfo->setSnapInterval(1);

        // Split the VFO output: one branch decodes, one branch produces audio.
        split.init(vfo->output);
        split.bindStream(&envStream);
        split.bindStream(&audioStream);

        // Decode branch: envelope -> decoder.
        dsp.init(&envStream, CW_DSP_SR, 80.0);
        envSink.init(&dsp.out, _envHandler, this);

        // Audio branch: CW beat note (BFO) -> resampler -> sink.
        audioDemod.init(&audioStream, tone, 100.0 / CW_DSP_SR, 5.0 / CW_DSP_SR, CW_DSP_SR);
        resamp.init(&audioDemod.out, CW_DSP_SR, audioSampleRate);
        srChangeHandler.ctx = this;
        srChangeHandler.handler = sampleRateChangeHandler;
        stream.init(&resamp.out, &srChangeHandler, audioSampleRate);
        sigpath::sinkManager.registerStream(name, &stream);

        // Start the processing graph.
        split.start();
        dsp.start();
        envSink.start();
        audioDemod.start();
        resamp.start();
        stream.start();

        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~CWDecoderModule() {
        gui::menu.removeEntry(name);
        stream.stop();
        if (enabled) {
            split.stop();
            dsp.stop();
            envSink.stop();
            audioDemod.stop();
            resamp.stop();
            sigpath::vfoManager.deleteVFO(vfo);
        }
        sigpath::sinkManager.unregisterStream(name);
    }

    void postInit() {}

    void enable() {
        double bw = gui::waterfall.getBandwidth();
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER,
                                            std::clamp<double>(0, -bw / 2.0, bw / 2.0),
                                            bandwidth, CW_DSP_SR, 50, 1000, false);
        vfo->setSnapInterval(1);
        split.setInput(vfo->output);
        split.start();
        dsp.start();
        envSink.start();
        audioDemod.start();
        resamp.start();
        enabled = true;
    }

    void disable() {
        split.stop();
        dsp.stop();
        envSink.stop();
        audioDemod.stop();
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
        config.conf[name]["speed"]        = speed;
        config.conf[name]["range"]        = range;
        config.conf[name]["tracking"]     = tracking;
        config.conf[name]["bandwidth"]    = bandwidth;
        config.conf[name]["squelchOn"]    = squelchOn;
        config.conf[name]["squelchLevel"] = squelchLevel;
        config.conf[name]["prosigns"]     = prosigns;
        config.conf[name]["tone"]         = tone;
        config.release(true);
    }

    void setAudioSampleRate(double sr) {
        audioSampleRate = sr;
        resamp.setOutSamplerate(audioSampleRate);
    }

    // Called from the DSP thread for every decoded character.
    void onChar(const std::string& s) {
        std::lock_guard<std::mutex> lck(textMtx);
        text += s;
        // Cap the on-screen buffer.
        if (text.size() > 16384) { text.erase(0, text.size() - 16384); }
        scrollToBottom = true;
    }

    static void _envHandler(float* data, int count, void* ctx) {
        CWDecoderModule* _this = (CWDecoderModule*)ctx;
        _this->decoder.process(data, count);
    }

    static void sampleRateChangeHandler(float sampleRate, void* ctx) {
        CWDecoderModule* _this = (CWDecoderModule*)ctx;
        _this->setAudioSampleRate(sampleRate);
    }

    static void menuHandler(void* ctx) {
        CWDecoderModule* _this = (CWDecoderModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) { style::beginDisabled(); }

        // --- Speed (WPM) ---
        ImGui::LeftLabel("Speed (WPM)");
        ImGui::FillWidth();
        if (ImGui::SliderInt(CONCAT("##cw_speed_", _this->name), &_this->speed,
                             cw::CW_LOWER_LIMIT, cw::CW_UPPER_LIMIT)) {
            _this->decoder.setSpeed(_this->speed);
            _this->saveConf();
        }

        // --- Speed range (+/- WPM), like FLDigi ---
        ImGui::LeftLabel("Range (+/- WPM)");
        ImGui::FillWidth();
        if (ImGui::SliderInt(CONCAT("##cw_range_", _this->name), &_this->range, 0, 30)) {
            _this->decoder.setRange(_this->range);
            _this->saveConf();
        }

        // --- Adaptive tracking ---
        if (ImGui::Checkbox(CONCAT("Speed tracking##cw_track_", _this->name),
                            &_this->tracking)) {
            _this->decoder.setTracking(_this->tracking);
            _this->saveConf();
        }

        // --- Channel bandwidth ---
        ImGui::LeftLabel("Filter (Hz)");
        ImGui::FillWidth();
        if (ImGui::SliderFloat(CONCAT("##cw_bw_", _this->name), &_this->bandwidth,
                               50.0f, 1000.0f, "%.0f")) {
            if (_this->enabled) { _this->vfo->setBandwidth(_this->bandwidth); }
            _this->saveConf();
        }

        // --- Audio tone (sidetone / BFO pitch) ---
        ImGui::LeftLabel("Tone (Hz)");
        ImGui::FillWidth();
        if (ImGui::SliderInt(CONCAT("##cw_tone_", _this->name), &_this->tone, 250, 1250)) {
            _this->audioDemod.setTone((double)_this->tone);
            _this->saveConf();
        }

        // --- Squelch ---
        if (ImGui::Checkbox(CONCAT("Squelch##cw_sql_", _this->name), &_this->squelchOn)) {
            _this->decoder.setSquelch(_this->squelchOn, _this->squelchLevel);
            _this->saveConf();
        }
        if (_this->squelchOn) {
            ImGui::SameLine();
            ImGui::FillWidth();
            if (ImGui::SliderFloat(CONCAT("##cw_sqllvl_", _this->name),
                                   &_this->squelchLevel, 0.0f, 100.0f, "%.0f")) {
                _this->decoder.setSquelch(_this->squelchOn, _this->squelchLevel);
                _this->saveConf();
            }
        }

        // --- Prosign display ---
        if (ImGui::Checkbox(CONCAT("Show prosigns##cw_ps_", _this->name),
                            &_this->prosigns)) {
            _this->decoder.setProsignDisplay(_this->prosigns);
            _this->saveConf();
        }

        // --- Status line: detected speed + signal level ---
        ImGui::Text("RX speed: %d WPM", _this->decoder.getReceiveSpeed());
        float lvl = (float)std::clamp(_this->decoder.getSigLevel(), 0.0, 1.0);
        ImGui::LeftLabel("Level");
        ImGui::FillWidth();
        ImGui::ProgressBar(lvl, ImVec2(0, 0));

        // --- Decoded text area ---
        ImGui::Text("Decoded text:");
        ImVec2 boxSize(menuWidth, 180.0f * style::uiScale);
        ImGui::BeginChild(CONCAT("##cw_text_", _this->name), boxSize, true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        {
            std::lock_guard<std::mutex> lck(_this->textMtx);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(_this->text.c_str());
            ImGui::PopTextWrapPos();
            if (_this->scrollToBottom) {
                ImGui::SetScrollHereY(1.0f);
                _this->scrollToBottom = false;
            }
        }
        ImGui::EndChild();

        if (ImGui::Button(CONCAT("Clear##cw_clear_", _this->name), ImVec2(menuWidth, 0))) {
            std::lock_guard<std::mutex> lck(_this->textMtx);
            _this->text.clear();
            _this->decoder.reset();
        }

        if (!_this->enabled) { style::endDisabled(); }
    }

    std::string name;
    bool enabled = true;

    VFOManager::VFO* vfo = NULL;

    // VFO output is split between the decode branch and the audio branch.
    dsp::routing::Splitter<dsp::complex_t> split;
    dsp::stream<dsp::complex_t> envStream;
    dsp::stream<dsp::complex_t> audioStream;

    // Decode branch.
    CWDSP dsp;
    dsp::sink::Handler<float> envSink;
    cw::Decoder decoder;

    // Audio branch (CW beat note -> sink).
    dsp::demod::CW<dsp::stereo_t> audioDemod;
    dsp::multirate::RationalResampler<dsp::stereo_t> resamp;
    SinkManager::Stream stream;
    EventHandler<float> srChangeHandler;
    double audioSampleRate = 48000.0;

    // GUI / state
    std::mutex textMtx;
    std::string text;
    bool scrollToBottom = false;

    // persisted settings
    int speed = 18;
    int range = 10;
    bool tracking = true;
    float bandwidth = 200.0f;
    bool squelchOn = false;
    float squelchLevel = 10.0f;
    bool prosigns = true;
    int tone = 600;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/cw_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new CWDecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (CWDecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
