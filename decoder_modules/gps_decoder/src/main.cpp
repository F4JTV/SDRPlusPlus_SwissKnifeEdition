// main.cpp -- SDR++ GPS L1 C/A decoder module
//
// Tuning: VFO is centred on 1575.42 MHz (GPS L1) with a 2.0 MHz bandwidth.
// The sink delivers complex IQ at 2.048 MHz. We use that rate end-to-end:
// FFT size 2048 perfectly fits one 1 ms code period and is FFTW-friendly.
//
// On the SDR++ side we are a normal decoder module: one VFO with a fixed
// bandwidth, an IQ sink fed by dsp::sink::Handler, and a menu in the left
// panel listing acquired satellites and their tracking state.

#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <dsp/sink/handler_sink.h>
#include <utils/flog.h>
#include <core.h>
#include <config.h>

#include <complex>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "gps_decoder.h"

SDRPP_MOD_INFO{
    /* Name:            */ "gps_decoder",
    /* Description:     */ "GPS L1 C/A (1575.42 MHz) Decoder",
    /* Author:          */ "SDR++ community",
    /* Version:         */ 1, 0, 0,
    /* Max instances    */ 1
};

ConfigManager config;

// L1 carrier frequency
static constexpr double GPS_L1_FREQ_HZ = 1575420000.0;

// VFO setup: SDR++ delivers samples at the requested VFO sample rate so
// long as the source supports it. 2.048 MHz is universally supported by
// the resampler.
static constexpr double VFO_BANDWIDTH = 2000000.0;     // 2 MHz of pass-band
static constexpr double VFO_SAMPLERATE = 2048000.0;    // 2.048 Msps

#define MAX_SUBFRAME_LOG 256

class GpsDecoderModule : public ModuleManager::Instance {
public:
    GpsDecoderModule(std::string name) {
        this->name = name;

        // Load persistent settings ------------------------------------------------
        config.acquire();
        if (!config.conf.contains(name)) config.conf[name] = json({});
        if (!config.conf[name].contains("acquisitionThreshold")) {
            config.conf[name]["acquisitionThreshold"] = 2.5f;
        }
        if (!config.conf[name].contains("acquisitionPeriodSec")) {
            config.conf[name]["acquisitionPeriodSec"] = 2.0f;
        }
        acquisitionThreshold_  = config.conf[name]["acquisitionThreshold"];
        acquisitionPeriodSec_  = config.conf[name]["acquisitionPeriodSec"];
        config.release(true);

        // Build the decoder -------------------------------------------------------
        decoder_ = std::make_unique<gps::GpsDecoder>(VFO_SAMPLERATE, acquisitionThreshold_);
        decoder_->setAcquisitionPeriodSec(acquisitionPeriodSec_);
        decoder_->setSubframeCallback([this](const gps::SubframeInfo& info) {
            this->onSubframe(info);
        });

        // Create a VFO. The bandwidth is locked at 2 MHz (signal main lobe
        // ±1.023 MHz around the carrier). We do not constrain the centre
        // frequency offset -- SDR++ centers the VFO wherever the user puts
        // it, which is normally on the L1 carrier within the waterfall.
        vfo_ = sigpath::vfoManager.createVFO(name,
                                             ImGui::WaterfallVFO::REF_CENTER,
                                             0,                  // offset
                                             VFO_BANDWIDTH,
                                             VFO_SAMPLERATE,
                                             VFO_BANDWIDTH,
                                             VFO_BANDWIDTH,
                                             true);
        // Mark the VFO with a distinctive colour (cyan)
        vfo_->setColor(IM_COL32(0, 200, 220, 60)); // low alpha so the waterfall stays visible behind the VFO

        // Wire up the sink that feeds the decoder
        sink_.init(vfo_->output, sinkHandler, this);
        sink_.start();

        decoder_->start();

        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~GpsDecoderModule() {
        gui::menu.removeEntry(name);
        if (decoder_) decoder_->stop();
        sink_.stop();
        if (vfo_) sigpath::vfoManager.deleteVFO(vfo_);
    }

    void postInit() override {}

    void enable() override {
        if (enabled_) return;
        vfo_ = sigpath::vfoManager.createVFO(name,
                                             ImGui::WaterfallVFO::REF_CENTER,
                                             0,
                                             VFO_BANDWIDTH,
                                             VFO_SAMPLERATE,
                                             VFO_BANDWIDTH,
                                             VFO_BANDWIDTH,
                                             true);
        vfo_->setColor(IM_COL32(0, 200, 220, 60)); // low alpha so the waterfall stays visible behind the VFO
        sink_.setInput(vfo_->output);
        sink_.start();
        decoder_->start();
        enabled_ = true;
    }

    void disable() override {
        if (!enabled_) return;
        decoder_->stop();
        sink_.stop();
        if (vfo_) {
            sigpath::vfoManager.deleteVFO(vfo_);
            vfo_ = nullptr;
        }
        enabled_ = false;
    }

    bool isEnabled() override { return enabled_; }

private:
    // ------------------------------------------------------------------------
    // DSP path: forward complex IQ from the VFO sink into the decoder
    // ------------------------------------------------------------------------
    static void sinkHandler(dsp::complex_t* data, int count, void* ctx) {
        auto* _this = (GpsDecoderModule*)ctx;
        // dsp::complex_t is already a std::complex<float>-compatible layout
        _this->decoder_->pushSamples(reinterpret_cast<std::complex<float>*>(data), count);
    }

    // ------------------------------------------------------------------------
    // Subframe callback: keep a small ring buffer for the GUI log panel
    // ------------------------------------------------------------------------
    void onSubframe(const gps::SubframeInfo& info) {
        std::lock_guard<std::mutex> l(logMu_);
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "PRN %2d  SF%d  TOW=%6u  %s%s",
                      info.prn, info.subframeId,
                      (unsigned)info.tow_count,
                      info.alertFlag      ? "ALERT " : "",
                      info.antispoofFlag  ? "AS"     : "");
        subframeLog_.push_back(std::string(buf));
        while (subframeLog_.size() > MAX_SUBFRAME_LOG) subframeLog_.pop_front();
    }

    // ------------------------------------------------------------------------
    // GUI
    // ------------------------------------------------------------------------
    static void menuHandler(void* ctx) {
        auto* _this = (GpsDecoderModule*)ctx;
        _this->drawMenu();
    }

    void drawMenu() {
        float w = ImGui::GetContentRegionAvail().x;

        // --- Tuning button: snap the radio to L1 -------------------------------
        if (ImGui::Button("Tune to L1 (1575.42 MHz)", ImVec2(w, 0))) {
            tuner::tune(tuner::TUNER_MODE_NORMAL, name, GPS_L1_FREQ_HZ);
        }

        ImGui::Separator();

        // --- Acquisition settings ---------------------------------------------
        ImGui::TextUnformatted("Acquisition");

        ImGui::LeftLabel("Threshold");
        ImGui::FillWidth();
        if (ImGui::SliderFloat(("##gps_acq_thr_" + name).c_str(),
                                &acquisitionThreshold_, 1.5f, 6.0f, "%.2f")) {
            decoder_->setAcquisitionThreshold(acquisitionThreshold_);
            saveConf("acquisitionThreshold", acquisitionThreshold_);
        }

        ImGui::LeftLabel("Period (s)");
        ImGui::FillWidth();
        if (ImGui::SliderFloat(("##gps_acq_per_" + name).c_str(),
                                &acquisitionPeriodSec_, 0.5f, 10.0f, "%.1f")) {
            decoder_->setAcquisitionPeriodSec(acquisitionPeriodSec_);
            saveConf("acquisitionPeriodSec", acquisitionPeriodSec_);
        }

        if (ImGui::Button("Acquire now", ImVec2(w * 0.5f - 4, 0))) {
            decoder_->forceAcquireNow();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset channels", ImVec2(w * 0.5f - 4, 0))) {
            decoder_->clearChannels();
        }

        ImGui::Separator();

        // --- Tracking channels table ------------------------------------------
        ImGui::TextUnformatted("Tracking channels");
        auto channels = decoder_->getChannelStates();
        ImGui::Text("Active: %d", (int)channels.size());

        if (ImGui::BeginTable(("gps_tracking_" + name).c_str(), 6,
                              ImGuiTableFlags_Borders |
                              ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("PRN");
            ImGui::TableSetupColumn("Doppler");
            ImGui::TableSetupColumn("C/N0");
            ImGui::TableSetupColumn("Lock");
            ImGui::TableSetupColumn("Bits");
            ImGui::TableSetupColumn("SubF");
            ImGui::TableHeadersRow();

            for (auto& c : channels) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%d", c.prn);
                ImGui::TableNextColumn(); ImGui::Text("%+5.0f Hz", c.dopplerHz);
                ImGui::TableNextColumn();
                if (c.cn0_dBHz >= 35.0f) {
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "%.1f", c.cn0_dBHz);
                } else if (c.cn0_dBHz >= 28.0f) {
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "%.1f", c.cn0_dBHz);
                } else {
                    ImGui::TextColored(ImVec4(0.85f, 0.4f, 0.4f, 1.0f), "%.1f", c.cn0_dBHz);
                }
                ImGui::TableNextColumn();
                ImGui::Text("%s", c.locked ? "yes" : "...");
                ImGui::TableNextColumn();
                ImGui::Text("%d", c.bitsDecoded);
                ImGui::TableNextColumn();
                ImGui::Text("%d", c.subframesDecoded);
            }
            ImGui::EndTable();
        }

        // --- Acquisition snapshot --------------------------------------------
        ImGui::Separator();
        ImGui::TextUnformatted("Last acquisition snapshot");
        auto acq = decoder_->getAcquisitionResults();
        int acquiredCount = 0;
        for (auto& a : acq) if (a.acquired) acquiredCount++;
        ImGui::Text("Acquired: %d / 32", acquiredCount);

        if (ImGui::BeginTable(("gps_acq_" + name).c_str(), 4,
                              ImGuiTableFlags_Borders |
                              ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Resizable,
                              ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 7))) {
            ImGui::TableSetupColumn("PRN");
            ImGui::TableSetupColumn("Doppler");
            ImGui::TableSetupColumn("Phase");
            ImGui::TableSetupColumn("Peak/Mean");
            ImGui::TableHeadersRow();

            for (auto& a : acq) {
                if (!a.acquired) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%d", a.prn);
                ImGui::TableNextColumn(); ImGui::Text("%+5.0f Hz", a.dopplerHz);
                ImGui::TableNextColumn(); ImGui::Text("%d", a.codePhase);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", a.peakMetric);
            }
            ImGui::EndTable();
        }

        // --- Subframe log -----------------------------------------------------
        ImGui::Separator();
        ImGui::TextUnformatted("Decoded subframes");
        {
            std::lock_guard<std::mutex> l(logMu_);
            if (ImGui::BeginChild(("gps_log_" + name).c_str(),
                                  ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 6),
                                  true)) {
                for (auto& line : subframeLog_) {
                    ImGui::TextUnformatted(line.c_str());
                }
                // Auto-scroll
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
                    ImGui::SetScrollHereY(1.0f);
                }
            }
            ImGui::EndChild();
        }
        if (ImGui::Button("Clear log", ImVec2(w, 0))) {
            std::lock_guard<std::mutex> l(logMu_);
            subframeLog_.clear();
        }
    }

    template<typename T>
    void saveConf(const std::string& key, T value) {
        config.acquire();
        config.conf[name][key] = value;
        config.release(true);
    }

    // ----- member state -----------------------------------------------------
    std::string name;
    bool enabled_ = true;

    VFOManager::VFO* vfo_ = nullptr;
    dsp::sink::Handler<dsp::complex_t> sink_;

    std::unique_ptr<gps::GpsDecoder> decoder_;

    float acquisitionThreshold_;
    float acquisitionPeriodSec_;

    std::deque<std::string> subframeLog_;
    std::mutex              logMu_;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/gps_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new GpsDecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (GpsDecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
