#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <utils/optionlist.h>
#include <cstdio>

#include "decoder.h"
#include "psk/decoder.h"
#include "psk/qpsk_decoder.h"
#include "psk/psk8_decoder.h"

SDRPP_MOD_INFO{
    /* Name:            */ "psk_decoder",
    /* Description:     */ "PSK family decoder (BPSK / QPSK / 8PSK, FLDIGI compatible)",
    /* Author:          */ "F4JTV",
    /* Version:         */ 0, 4, 0,
    /* Max instances    */ -1
};

ConfigManager config;

// Modes are grouped into three contiguous ranges:
//   [BPSK31  .. BPSK1000]   -> BPSKDecoder, idx = mode - BPSK31
//   [QPSK31  .. QPSK500]    -> QPSKDecoder, idx = mode - QPSK31
//   [PSK8_125 .. PSK8_1000] -> PSK8Decoder, idx = mode - PSK8_125
enum FldigiMode {
    FLDIGI_MODE_INVALID = -1,
    FLDIGI_MODE_BPSK31   = 0,
    FLDIGI_MODE_BPSK63,
    FLDIGI_MODE_BPSK63F,
    FLDIGI_MODE_BPSK125,
    FLDIGI_MODE_BPSK250,
    FLDIGI_MODE_BPSK500,
    FLDIGI_MODE_BPSK1000,
    FLDIGI_MODE_QPSK31,
    FLDIGI_MODE_QPSK63,
    FLDIGI_MODE_QPSK125,
    FLDIGI_MODE_QPSK250,
    FLDIGI_MODE_QPSK500,
    FLDIGI_MODE_PSK8_125,
    FLDIGI_MODE_PSK8_250,
    FLDIGI_MODE_PSK8_500,
    FLDIGI_MODE_PSK8_1000
};

static inline bool isBPSKMode(FldigiMode m) {
    return m >= FLDIGI_MODE_BPSK31 && m <= FLDIGI_MODE_BPSK1000;
}
static inline bool isQPSKMode(FldigiMode m) {
    return m >= FLDIGI_MODE_QPSK31 && m <= FLDIGI_MODE_QPSK500;
}
static inline bool isPSK8Mode(FldigiMode m) {
    return m >= FLDIGI_MODE_PSK8_125 && m <= FLDIGI_MODE_PSK8_1000;
}

class FldigiDecoderModule : public ModuleManager::Instance {
public:
    FldigiDecoderModule(std::string name) {
        this->name = name;

        // Mode option list (display labels + enum)
        fldigiModes.define("BPSK31",   FLDIGI_MODE_BPSK31);
        fldigiModes.define("BPSK63",   FLDIGI_MODE_BPSK63);
        fldigiModes.define("BPSK63F",  FLDIGI_MODE_BPSK63F);
        fldigiModes.define("BPSK125",  FLDIGI_MODE_BPSK125);
        fldigiModes.define("BPSK250",  FLDIGI_MODE_BPSK250);
        fldigiModes.define("BPSK500",  FLDIGI_MODE_BPSK500);
        fldigiModes.define("BPSK1000", FLDIGI_MODE_BPSK1000);
        fldigiModes.define("QPSK31",   FLDIGI_MODE_QPSK31);
        fldigiModes.define("QPSK63",   FLDIGI_MODE_QPSK63);
        fldigiModes.define("QPSK125",  FLDIGI_MODE_QPSK125);
        fldigiModes.define("QPSK250",  FLDIGI_MODE_QPSK250);
        fldigiModes.define("QPSK500",  FLDIGI_MODE_QPSK500);
        fldigiModes.define("8PSK125",  FLDIGI_MODE_PSK8_125);
        fldigiModes.define("8PSK250",  FLDIGI_MODE_PSK8_250);
        fldigiModes.define("8PSK500",  FLDIGI_MODE_PSK8_500);
        fldigiModes.define("8PSK1000", FLDIGI_MODE_PSK8_1000);
        vfoModes.define("USB", VFO_MODE_USB);
        vfoModes.define("LSB", VFO_MODE_LSB);
        vfoModes.define("NFM", VFO_MODE_NFM);
        snapIntervals.define("1 Hz",      1.0);
        snapIntervals.define("100 Hz",    100.0);
        snapIntervals.define("500 Hz",    500.0);
        snapIntervals.define("1 kHz",     1000.0);
        snapIntervals.define("2.5 kHz",   2500.0);
        snapIntervals.define("5 kHz",     5000.0);
        snapIntervals.define("12.5 kHz",  12500.0);

        // Load config
        config.acquire();
        if (config.conf[name].contains("mode")) {
            std::string m = config.conf[name]["mode"];
            if (fldigiModes.keyExists(m)) { modeId = fldigiModes.keyId(m); }
        }
        if (config.conf[name].contains("vfoMode")) {
            std::string m = config.conf[name]["vfoMode"];
            if (vfoModes.keyExists(m)) { vfoModeId = vfoModes.keyId(m); }
        }
        if (config.conf[name].contains("afFreq")) {
            afFreq = config.conf[name]["afFreq"];
        }
        if (config.conf[name].contains("squelchEnabled")) {
            squelchEnabled = config.conf[name]["squelchEnabled"];
        }
        if (config.conf[name].contains("squelchLevel")) {
            squelchLevel = config.conf[name]["squelchLevel"];
        }
        if (config.conf[name].contains("snapInterval")) {
            std::string s = config.conf[name]["snapInterval"];
            if (snapIntervals.keyExists(s)) { snapIntervalId = snapIntervals.keyId(s); }
        }
        config.release();

        // Create the VFO. The decoder will adjust reference/bandwidth/sample
        // rate to match the selected VFO mode (USB/LSB/NFM). We pick sane
        // initial values for USB (the default mode).
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_LOWER, 0, 2800, 24000, 2800, 2800, true);
        vfo->setSnapInterval(snapIntervals.value(snapIntervalId));

        // Create the active decoder
        selectMode(fldigiModes.value(modeId));

        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~FldigiDecoderModule() {
        gui::menu.removeEntry(name);
        if (enabled) {
            decoder->stop();
            decoder.reset();
            sigpath::vfoManager.deleteVFO(vfo);
        }
        sigpath::sinkManager.unregisterStream(name);
    }

    void postInit() {}

    void enable() {
        double bw = gui::waterfall.getBandwidth();
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_LOWER,
                                            std::clamp<double>(0, -bw / 2.0, bw / 2.0),
                                            2800, 24000, 2800, 2800, true);
        vfo->setSnapInterval(snapIntervals.value(snapIntervalId));
        selectMode(mode, true);
        enabled = true;
    }

    void disable() {
        if (decoder) {
            decoder->stop();
            decoder.reset();
        }
        sigpath::vfoManager.deleteVFO(vfo);
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    void selectMode(FldigiMode newMode, bool force = false) {
        if (!force && newMode == mode) { return; }
        if (newMode == FLDIGI_MODE_INVALID) {
            flog::error("FLDIGI: unknown mode selected ({})", (int)newMode);
            return;
        }

        if (decoder) {
            decoder->stop();
            decoder.reset();
        }

        if (isBPSKMode(newMode)) {
            int idx = (int)newMode - (int)FLDIGI_MODE_BPSK31;
            decoder = std::make_unique<BPSKDecoder>(name, vfo, idx);
        }
        else if (isQPSKMode(newMode)) {
            int idx = (int)newMode - (int)FLDIGI_MODE_QPSK31;
            decoder = std::make_unique<QPSKDecoder>(name, vfo, idx);
        }
        else if (isPSK8Mode(newMode)) {
            int idx = (int)newMode - (int)FLDIGI_MODE_PSK8_125;
            decoder = std::make_unique<PSK8Decoder>(name, vfo, idx);
        }
        else {
            flog::error("PSK: mode {} not in any handled range", (int)newMode);
            return;
        }

        // Apply current VFO mode / AF / squelch settings
        decoder->setVfoMode(vfoModes.value(vfoModeId));
        decoder->setAFFreq(afFreq);
        decoder->setSquelchLevel(squelchLevel);
        decoder->setSquelchEnabled(squelchEnabled);
        decoder->start();

        mode = newMode;
    }

    static void menuHandler(void* ctx) {
        FldigiDecoderModule* _this = (FldigiDecoderModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) { style::beginDisabled(); }

        // FLDIGI mode
        ImGui::LeftLabel("Mode");
        ImGui::FillWidth();
        if (ImGui::Combo(("##fldigi_mode_" + _this->name).c_str(), &_this->modeId, _this->fldigiModes.txt)) {
            _this->selectMode(_this->fldigiModes.value(_this->modeId));
            config.acquire();
            config.conf[_this->name]["mode"] = _this->fldigiModes.key(_this->modeId);
            config.release(true);
        }

        // VFO sideband / demod
        ImGui::LeftLabel("VFO mode");
        ImGui::FillWidth();
        if (ImGui::Combo(("##fldigi_vfomode_" + _this->name).c_str(), &_this->vfoModeId, _this->vfoModes.txt)) {
            if (_this->decoder) { _this->decoder->setVfoMode(_this->vfoModes.value(_this->vfoModeId)); }
            config.acquire();
            config.conf[_this->name]["vfoMode"] = _this->vfoModes.key(_this->vfoModeId);
            config.release(true);
        }

        // Snap interval applied to the VFO when dragging it on the waterfall
        ImGui::LeftLabel("Snap");
        ImGui::FillWidth();
        if (ImGui::Combo(("##fldigi_snap_" + _this->name).c_str(), &_this->snapIntervalId, _this->snapIntervals.txt)) {
            if (_this->vfo) { _this->vfo->setSnapInterval(_this->snapIntervals.value(_this->snapIntervalId)); }
            config.acquire();
            config.conf[_this->name]["snapInterval"] = _this->snapIntervals.key(_this->snapIntervalId);
            config.release(true);
        }

        // --- Band view: smoothed audio-passband spectrum with a marker
        //     showing the current AF freq. Click or drag to set the AF.
        //     Same approach as the MFSK module's band view. ---
        if (_this->decoder) {
            static float spec[256];
            int nb = _this->decoder->getBandSpectrum(spec, 256);
            if (nb > 0) {
                double flo  = _this->decoder->getBandFlo();
                double fhi  = _this->decoder->getBandFhi();
                double bw   = _this->decoder->getSignalBandwidth();
                double afc  = (double)_this->decoder->getAFFreq();

                float w = menuWidth;
                float h = 56.0f;
                ImVec2 p0 = ImGui::GetCursorScreenPos();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImU32 cBg     = IM_COL32(20, 22, 28, 255);
                ImU32 cBar    = IM_COL32(90, 160, 230, 255);
                ImU32 cBand   = IM_COL32(90, 160, 230, 60);
                ImU32 cMarker = IM_COL32(250, 200, 80, 255);
                dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), cBg, 3.0f);

                // Normalise to max for display
                float mx = 1e-6f;
                for (int i = 0; i < nb; i++) { if (spec[i] > mx) mx = spec[i]; }

                // Frequency-to-x helper
                auto f2x = [&](double f) {
                    return p0.x + w * (float)((f - flo) / (fhi - flo));
                };

                // Shaded band covering [AF - bw/2, AF + bw/2]: this is the
                // expected signal footprint. Drawn under the bars so the
                // user sees both the trace and where the decoder is "aimed".
                if (bw > 0) {
                    float xa = f2x(afc - bw * 0.5);
                    float xb = f2x(afc + bw * 0.5);
                    if (xa < p0.x)       xa = p0.x;
                    if (xb > p0.x + w)   xb = p0.x + w;
                    if (xb > xa) {
                        dl->AddRectFilled(ImVec2(xa, p0.y), ImVec2(xb, p0.y + h), cBand);
                    }
                }

                // Spectrum bars
                for (int i = 0; i < nb; i++) {
                    float x  = p0.x + w * (i / (float)(nb - 1));
                    float bh = (spec[i] / mx) * (h - 4);
                    dl->AddLine(ImVec2(x, p0.y + h - 2), ImVec2(x, p0.y + h - 2 - bh), cBar, 1.0f);
                }

                // AF marker line
                float xe = f2x(afc);
                dl->AddLine(ImVec2(xe, p0.y), ImVec2(xe, p0.y + h), cMarker, 2.0f);

                // Click / drag to set AF
                ImGui::InvisibleButton(("##psk_band_" + _this->name).c_str(), ImVec2(w, h));
                if (ImGui::IsItemActive()) {
                    float mxs = ImGui::GetIO().MousePos.x;
                    double f = flo + (double)((mxs - p0.x) / w) * (fhi - flo);
                    f = std::round(f);
                    if (f < 700.0)  { f = 700.0; }
                    if (f > 2500.0) { f = 2500.0; }
                    _this->afFreq = (float)f;
                    _this->decoder->setAFFreq(_this->afFreq);
                }
                if (ImGui::IsItemDeactivatedAfterEdit() ||
                    (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)))
                {
                    config.acquire();
                    config.conf[_this->name]["afFreq"] = _this->afFreq;
                    config.release(true);
                }
            }
        }

        // Audio passband frequency (FLDIGI cursor)
        ImGui::LeftLabel("AF freq");
        ImGui::FillWidth();
        if (ImGui::SliderFloat(("##fldigi_aff_" + _this->name).c_str(), &_this->afFreq, 700.0f, 2500.0f, "%.0f Hz")) {
            if (_this->decoder) { _this->decoder->setAFFreq(_this->afFreq); }
            config.acquire();
            config.conf[_this->name]["afFreq"] = _this->afFreq;
            config.release(true);
        }

        // Auto-tune AF frequency. Sweeps the SSB passband with a Welch-
        // segmented Goertzel bank over ~2.5 s of audio and snaps AF freq to
        // the energy centroid of the strongest spectral peak. Same general
        // approach as the MFSK module's auto-acquisition.
        bool tuning = _this->decoder && _this->decoder->isAutoTuning();
        if (tuning) {
            // Show progress bar in place of the button while scanning
            float p = _this->decoder->autoTuneProgress();
            char lbl[32];
            std::snprintf(lbl, sizeof(lbl), "Auto-tuning... %d%%", (int)(p * 100.0f));
            ImGui::ProgressBar(p, ImVec2(menuWidth, 0), lbl);

            // If the scan finished, fetch the result, update slider + config
            if (_this->decoder->autoTuneReady()) {
                float af = _this->decoder->autoTuneResult();
                if (af > 0.0f) {
                    _this->afFreq = af;
                    config.acquire();
                    config.conf[_this->name]["afFreq"] = _this->afFreq;
                    config.release(true);
                }
            }
        }
        else {
            if (ImGui::Button(("Auto AF##fldigi_at_" + _this->name).c_str(), ImVec2(menuWidth, 0))) {
                if (_this->decoder) { _this->decoder->beginAutoTune(); }
            }
        }

        // Squelch (gates the IQ before demodulation, same approach as the
        // SDR++ Radio module's power squelch)
        if (ImGui::Checkbox(("Squelch##fldigi_sq_en_" + _this->name).c_str(), &_this->squelchEnabled)) {
            if (_this->decoder) { _this->decoder->setSquelchEnabled(_this->squelchEnabled); }
            config.acquire();
            config.conf[_this->name]["squelchEnabled"] = _this->squelchEnabled;
            config.release(true);
        }
        if (!_this->squelchEnabled) { style::beginDisabled(); }
        ImGui::FillWidth();
        if (ImGui::SliderFloat(("##fldigi_sq_lvl_" + _this->name).c_str(), &_this->squelchLevel, -100.0f, 0.0f, "%.1f dB")) {
            if (_this->decoder) { _this->decoder->setSquelchLevel(_this->squelchLevel); }
            config.acquire();
            config.conf[_this->name]["squelchLevel"] = _this->squelchLevel;
            config.release(true);
        }
        if (!_this->squelchEnabled) { style::endDisabled(); }

        // Mode-specific UI (scope + decoded text)
        if (_this->decoder) { _this->decoder->showMenu(); }

        if (!_this->enabled) { style::endDisabled(); }
    }

    std::string name;
    bool enabled = true;

    VFOManager::VFO* vfo = NULL;
    std::unique_ptr<Decoder> decoder;

    FldigiMode mode = FLDIGI_MODE_INVALID;
    int modeId = 0;
    int vfoModeId = 0;
    int snapIntervalId = 0;  // default = 1 Hz (first entry)
    float afFreq = 1000.0f;
    bool  squelchEnabled = false;
    float squelchLevel   = -50.0f;

    OptionList<std::string, FldigiMode> fldigiModes;
    OptionList<std::string, VfoMode> vfoModes;
    OptionList<std::string, double> snapIntervals;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/psk_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new FldigiDecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (FldigiDecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
