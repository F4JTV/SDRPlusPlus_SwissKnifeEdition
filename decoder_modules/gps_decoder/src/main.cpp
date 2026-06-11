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

#include <chrono>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "gps_decoder.h"
#include "gps_time.h"

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
        if (!config.conf[name].contains("leapSeconds")) {
            config.conf[name]["leapSeconds"] = gps::DEFAULT_LEAP_SECONDS;
        }
        acquisitionThreshold_  = config.conf[name]["acquisitionThreshold"];
        acquisitionPeriodSec_  = config.conf[name]["acquisitionPeriodSec"];
        leapSeconds_           = config.conf[name]["leapSeconds"];
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

        if (ImGui::BeginTable(("gps_tracking_" + name).c_str(), 7,
                              ImGuiTableFlags_Borders |
                              ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("PRN");
            ImGui::TableSetupColumn("Doppler");
            ImGui::TableSetupColumn("C/N0");
            ImGui::TableSetupColumn("Lock");
            ImGui::TableSetupColumn("Bits");
            ImGui::TableSetupColumn("SubF");
            ImGui::TableSetupColumn("Full");
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
                ImGui::TableNextColumn();
                ImGui::Text("%d", c.fullSubframesDecoded);
            }
            ImGui::EndTable();
        }

        // --- Time Sync --------------------------------------------------------
        ImGui::Separator();
        ImGui::TextUnformatted("Time Sync (GPS UTC)");

        gps::TimeFix fix = decoder_->getLatestTimeFix();
        if (!fix.valid) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                               "Status: waiting for a subframe with C/N0 >= 30 dB-Hz...");
        } else {
            auto now = std::chrono::system_clock::now();
            auto gpsUtc = gps::gpsTowToUtc(fix, now, leapSeconds_);
            auto offsetMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now - gpsUtc).count();

            ImGui::Text("Status: locked on PRN %d (C/N0 %.1f dB-Hz)",
                        fix.prn, fix.cn0_dBHz);
            ImGui::Text("GPS UTC:  %s", gps::formatUtcIso(gpsUtc).c_str());
            ImGui::Text("PC clock: %s", gps::formatUtcIso(now).c_str());

            ImVec4 col;
            long long absOff = std::llabs(offsetMs);
            if      (absOff <  100) col = ImVec4(0.2f, 1.0f, 0.2f, 1.0f);
            else if (absOff < 1000) col = ImVec4(1.0f, 0.85f, 0.2f, 1.0f);
            else                    col = ImVec4(0.85f, 0.4f, 0.4f, 1.0f);
            ImGui::TextColored(col,
                "PC - GPS = %+lld ms  (PC is %s)",
                (long long)offsetMs,
                offsetMs >= 0 ? "ahead" : "behind");
        }

        ImGui::LeftLabel("Leap seconds");
        ImGui::FillWidth();
        if (ImGui::InputInt(("##gps_leap_" + name).c_str(), &leapSeconds_)) {
            if (leapSeconds_ <  0) leapSeconds_ = 0;
            if (leapSeconds_ > 30) leapSeconds_ = 30;
            saveConf("leapSeconds", leapSeconds_);
        }

        const bool canSet = fix.valid;
        if (!canSet) ImGui::BeginDisabled();
        if (ImGui::Button(("Set system clock##gps_setclock_" + name).c_str(),
                          ImVec2(w, 0))) {
            auto target = gps::gpsTowToUtc(fix,
                                            std::chrono::system_clock::now(),
                                            leapSeconds_);
            std::string err;
            if (gps::setSystemClock(target, err)) {
                lastClockSetError_.clear();
                lastClockSetSuccess_ = std::chrono::system_clock::now();
            } else {
                lastClockSetError_ = err;
                lastClockSetSuccess_ = {};
            }
        }
        if (!canSet) ImGui::EndDisabled();

        if (!lastClockSetError_.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.5f, 1.0f),
                               "%s", lastClockSetError_.c_str());
            ImGui::PopTextWrapPos();
        } else if (lastClockSetSuccess_.time_since_epoch().count() != 0) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now() - lastClockSetSuccess_).count();
            if (age < 30) {
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f),
                                   "Clock set successfully %lld s ago",
                                   (long long)age);
            }
        }

        // --- Position Fix (PVT) ----------------------------------------------
        ImGui::Separator();
        ImGui::TextUnformatted("Position Fix (PVT)");

        // Per-PRN ephemeris-assembly progress
        auto ephStatus = decoder_->getEphemerisStatus();
        int n_eph_ready = 0;
        for (auto& s : ephStatus) if (s.consistent && s.cn0_dBHz >= 30.0f) n_eph_ready++;
        ImGui::Text("Satellites with usable ephemeris: %d", n_eph_ready);

        if (ImGui::BeginTable(("gps_eph_" + name).c_str(), 5,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("PRN");
            ImGui::TableSetupColumn("SF1");
            ImGui::TableSetupColumn("SF2");
            ImGui::TableSetupColumn("SF3");
            ImGui::TableSetupColumn("Ready");
            ImGui::TableHeadersRow();
            ImVec4 green(0.2f, 1.0f, 0.2f, 1.0f);
            ImVec4 grey(0.5f, 0.5f, 0.5f, 1.0f);
            for (auto& s : ephStatus) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%d", s.prn);
                ImGui::TableNextColumn();
                ImGui::TextColored(s.sf1 ? green : grey, "%s", s.sf1 ? "yes" : "...");
                ImGui::TableNextColumn();
                ImGui::TextColored(s.sf2 ? green : grey, "%s", s.sf2 ? "yes" : "...");
                ImGui::TableNextColumn();
                ImGui::TextColored(s.sf3 ? green : grey, "%s", s.sf3 ? "yes" : "...");
                ImGui::TableNextColumn();
                bool ready = s.consistent && s.cn0_dBHz >= 30.0f;
                ImGui::TextColored(ready ? green : grey, "%s",
                                   ready ? "YES" : (s.consistent ? "low C/N0" : "no"));
            }
            ImGui::EndTable();
        }

        ImGui::Checkbox(("Continuous PVT##gps_pvt_cont_" + name).c_str(),
                        &pvtContinuous_);
        ImGui::SameLine();
        const bool canPvt = (n_eph_ready >= 4);
        if (!canPvt) ImGui::BeginDisabled();
        if (ImGui::Button(("Compute fix##gps_pvt_now_" + name).c_str(),
                          ImVec2(w * 0.55f, 0))
            || (pvtContinuous_ && canPvt)) {
            lastPvtSolution_ = decoder_->solvePvtFix();
        }
        if (!canPvt) ImGui::EndDisabled();
        if (!canPvt) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                "Need >= 4 satellites with complete & consistent ephemeris and C/N0 >= 30 dB-Hz.");
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                "Each ephemeris takes ~30 s to assemble (3 subframes x 6 s + alignment).");
        }

        if (lastPvtSolution_.valid) {
            ImGui::Text("Satellites used : %d  (iter %d, residual %.1f m)",
                        lastPvtSolution_.used_sats,
                        lastPvtSolution_.iterations,
                        lastPvtSolution_.residual_rms_m);
            ImGui::Text("Latitude        : %+10.6f deg",  lastPvtSolution_.lla.lat_deg);
            ImGui::Text("Longitude       : %+10.6f deg",  lastPvtSolution_.lla.lon_deg);
            ImGui::Text("Altitude (WGS84): %+8.1f m",     lastPvtSolution_.lla.alt_m);
            ImGui::Text("ECEF x/y/z (m)  : %+.1f / %+.1f / %+.1f",
                        lastPvtSolution_.x, lastPvtSolution_.y, lastPvtSolution_.z);
            ImGui::Text("Clock bias      : %+.6e s",      lastPvtSolution_.clock_bias_s);
            ImGui::Text("DOP G/P/H/V/T   : %.1f / %.1f / %.1f / %.1f / %.1f",
                        lastPvtSolution_.gdop, lastPvtSolution_.pdop,
                        lastPvtSolution_.hdop, lastPvtSolution_.vdop,
                        lastPvtSolution_.tdop);
        } else if (lastPvtSolution_.used_sats > 0) {
            ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.5f, 1.0f),
                "Solver did not converge (used %d sats).",
                lastPvtSolution_.used_sats);
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
    int   leapSeconds_ = gps::DEFAULT_LEAP_SECONDS;

    // Last attempt to push GPS time to the OS clock
    std::string lastClockSetError_;
    std::chrono::system_clock::time_point lastClockSetSuccess_{};

    // PVT state
    gps::PvtSolution lastPvtSolution_;
    bool             pvtContinuous_ = false;

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
