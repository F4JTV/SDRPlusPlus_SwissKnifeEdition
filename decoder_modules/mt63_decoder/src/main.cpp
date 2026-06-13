#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <utils/optionlist.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include "decoder.h"
#include "mt63/decoder.h"

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "mt63_decoder",
    /* Description:     */ "MT63 decoder (500/1000/2000 Hz, short/long interleave, Jalocha/FLDIGI engine)",
    /* Author:          */ "F4JTV",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

ConfigManager config;

#define VFO_SAMPLE_RATE  (mt63::MT63_SR)   // 8 kHz: SSB demod -> 8 kHz audio -> engine
#define VFO_BANDWIDTH    3000.0

class Mt63DecoderModule : public ModuleManager::Instance {
public:
    Mt63DecoderModule(std::string name) {
        this->name = name;

        snapIntervals.define(1,    "1 Hz",    1.0);
        snapIntervals.define(10,   "10 Hz",   10.0);
        snapIntervals.define(50,   "50 Hz",   50.0);
        snapIntervals.define(100,  "100 Hz",  100.0);
        snapIntervals.define(250,  "250 Hz",  250.0);

        // Load config (defensive against a corrupt file left by a prior crash).
        config.acquire();
        try {
            auto& c = config.conf[name];
            if (c.contains("vfoMode")        && c["vfoMode"].is_number())        { vfoMode        = c["vfoMode"]; }
            if (c.contains("modeIdx")        && c["modeIdx"].is_number())        { modeIdx        = c["modeIdx"]; }
            if (c.contains("snapId")         && c["snapId"].is_number())         { snapId         = c["snapId"]; }
            if (c.contains("centerHz")       && c["centerHz"].is_number())       { centerHz       = c["centerHz"]; }
            if (c.contains("integ")          && c["integ"].is_number())          { integ          = c["integ"]; }
            if (c.contains("eightBit")       && c["eightBit"].is_boolean())      { eightBit       = c["eightBit"]; }
            if (c.contains("squelchEnabled") && c["squelchEnabled"].is_boolean()){ squelchEnabled = c["squelchEnabled"]; }
            if (c.contains("squelchLevel")   && c["squelchLevel"].is_number())   { squelchLevel   = c["squelchLevel"]; }
        }
        catch (...) { /* ignore bad config, fall back to defaults */ }
        config.release();

        vfoMode  = std::clamp(vfoMode, 0, 1);
        modeIdx  = std::clamp(modeIdx, 0, mt63::MT63_MODE_COUNT - 1);
        snapId   = std::clamp(snapId, 0, snapIntervals.size() - 1);
        integ    = std::clamp(integ, 8, 64);

        // Create the VFO at 8 kHz (REF_LOWER for USB so the passband is above
        // the marker; REF_UPPER for LSB so it is below).
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_LOWER, 0,
                                            VFO_BANDWIDTH, VFO_SAMPLE_RATE, 200.0, 4000.0, false);
        if (!vfo) { return; }
        vfo->setSnapInterval(snapIntervals.value(snapId));

        decoder = new mt63::Mt63Decoder(vfo->output);
        decoder->onChar = [this](char c) { onChar(c); };
        decoder->setVFOMode((mt63::VfoMode)vfoMode);
        applyVfoGeometry();
        decoder->setMode(modeIdx);
        decoder->setIntegration(integ);
        decoder->set8bit(eightBit);
        decoder->setAFFreq(centerHz);
        decoder->setSquelchEnabled(squelchEnabled);
        decoder->setSquelchLevel(squelchLevel);
        decoder->start();

        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~Mt63DecoderModule() {
        gui::menu.removeEntry(name);
        if (decoder) { decoder->stop(); delete decoder; decoder = nullptr; }
        if (vfo) { sigpath::vfoManager.deleteVFO(vfo); vfo = nullptr; }
    }

    void postInit() {}

    void enable() {
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_LOWER, 0,
                                            VFO_BANDWIDTH, VFO_SAMPLE_RATE, 200.0, 4000.0, false);
        if (!vfo) { return; }
        vfo->setSnapInterval(snapIntervals.value(snapId));
        applyVfoGeometry();
        if (decoder) {
            decoder->setInput(vfo->output);
            decoder->start();
        }
        enabled = true;
    }

    void disable() {
        if (decoder) { decoder->stop(); }
        if (vfo) { sigpath::vfoManager.deleteVFO(vfo); vfo = nullptr; }
        enabled = false;
    }

    bool isEnabled() { return enabled; }

    // USB -> marker on the lower edge (passband above); LSB -> marker on the
    // upper edge (passband below). MT63 is conventionally USB.
    void applyVfoGeometry() {
        if (!vfo) { return; }
        vfo->setBandwidthLimits(200.0, 4000.0, false);
        if (vfoMode == mt63::VFO_MODE_LSB) {
            vfo->setReference(ImGui::WaterfallVFO::REF_UPPER);
        }
        else {
            vfo->setReference(ImGui::WaterfallVFO::REF_LOWER);
        }
        vfo->setBandwidth(mt63::MT63_SSB_BW);
    }

private:
    void onChar(char c) {
        std::lock_guard<std::mutex> lck(textMtx);
        // Keep the text box readable: printable ASCII, plus CR/LF/TAB.
        bool keep = (c == '\n' || c == '\r' || c == '\t' ||
                     ((unsigned char)c >= 32));
        if (!keep) { return; }
        if (c == '\r') { return; }
        decodedText += c;
        if (decodedText.size() > 16384) {
            decodedText.erase(0, decodedText.size() - 16384);
        }
    }

    static void menuHandler(void* ctx) {
        Mt63DecoderModule* _this = (Mt63DecoderModule*)ctx;
        if (!_this->decoder) { return; }
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) { style::beginDisabled(); }

        // --- Mode -------------------------------------------------------------
        ImGui::LeftLabel("Mode");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(CONCAT("##mt63_mode_", _this->name), &_this->modeIdx, modesStr())) {
            _this->modeIdx = std::clamp(_this->modeIdx, 0, mt63::MT63_MODE_COUNT - 1);
            _this->decoder->setMode(_this->modeIdx);
            _this->saveConfig();
        }

        // --- VFO mode (sideband) ---------------------------------------------
        ImGui::LeftLabel("VFO mode");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(CONCAT("##mt63_vfomode_", _this->name), &_this->vfoMode, "USB\0LSB\0")) {
            _this->vfoMode = std::clamp(_this->vfoMode, 0, 1);
            _this->decoder->setVFOMode((mt63::VfoMode)_this->vfoMode);
            _this->applyVfoGeometry();
            _this->saveConfig();
        }

        // --- Snap interval ----------------------------------------------------
        ImGui::LeftLabel("Snap");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(CONCAT("##mt63_snap_", _this->name), &_this->snapId, _this->snapIntervals.txt)) {
            if (_this->vfo) { _this->vfo->setSnapInterval(_this->snapIntervals.value(_this->snapId)); }
            _this->saveConfig();
        }

        // --- Band view: audio passband spectrum with the MT63 signal band
        //     (centre-bw/2 .. centre+bw/2) shaded and a centre marker. Click or
        //     drag to set the signal centre. ----------------------------------
        {
            static float spec[256];
            int nb = _this->decoder->getBandSpectrum(spec, 256);
            double flo  = _this->decoder->getBandFlo();
            double fhi  = _this->decoder->getBandFhi();
            double edge = _this->decoder->getEdgeFreq();
            double span = _this->decoder->getToneSpan();
            double center = _this->centerHz;

            float w = menuWidth;
            float h = 56.0f;
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImU32 cBg   = IM_COL32(20, 22, 28, 255);
            ImU32 cBar  = IM_COL32(90, 160, 230, 255);
            ImU32 cBand = IM_COL32(90, 160, 230, 60);
            ImU32 cCtr  = IM_COL32(250, 200, 80, 255);
            dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), cBg, 3.0f);

            float mx = 1e-6f; for (int i = 0; i < nb; i++) mx = std::max(mx, spec[i]);
            for (int i = 0; i < nb; i++) {
                float x = p0.x + w * (i / (float)(nb - 1));
                float bh = (spec[i] / mx) * (h - 4);
                dl->AddLine(ImVec2(x, p0.y + h - 2), ImVec2(x, p0.y + h - 2 - bh), cBar, 1.0f);
            }
            auto f2x = [&](double f) { return p0.x + w * (float)((f - flo) / (fhi - flo)); };
            if (span > 0) {
                float xa = f2x(edge), xb = f2x(edge + span);
                dl->AddRectFilled(ImVec2(xa, p0.y), ImVec2(xb, p0.y + h), cBand);
            }
            float xc = f2x(center);
            dl->AddLine(ImVec2(xc, p0.y), ImVec2(xc, p0.y + h), cCtr, 2.0f);

            ImGui::InvisibleButton(CONCAT("##mt63_band_", _this->name), ImVec2(w, h));
            if (ImGui::IsItemActive()) {
                float mxs = ImGui::GetIO().MousePos.x;
                double f = flo + (double)((mxs - p0.x) / w) * (fhi - flo);
                f = std::round(f);
                if (f < 400) f = 400; if (f > 2400) f = 2400;
                _this->centerHz = f;
                _this->decoder->setAFFreq(_this->centerHz);
            }
            if (ImGui::IsItemDeactivatedAfterEdit() || (ImGui::IsItemHovered() && ImGui::IsMouseReleased(0)))
                _this->saveConfig();
        }

        // --- Centre frequency + auto-center ----------------------------------
        ImGui::LeftLabel("Center");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::SliderFloat(CONCAT("##mt63_center_", _this->name), &_this->centerHz,
                               400.0f, 2400.0f, "%.0f Hz")) {
            _this->decoder->setAFFreq(_this->centerHz);
            _this->saveConfig();
        }
        if (ImGui::Button(CONCAT("Auto-center##mt63_ac_", _this->name), ImVec2(menuWidth, 0))) {
            double est = _this->decoder->estimateCenter();
            if (est > 0) {
                _this->centerHz = (float)std::clamp(est, 400.0, 2400.0);
                _this->decoder->setAFFreq(_this->centerHz);
                _this->saveConfig();
            }
        }

        // --- FEC integration depth + 8-bit (advanced) ------------------------
        if (ImGui::CollapsingHeader(CONCAT("Decoder settings##mt63_dec_", _this->name))) {
            ImGui::LeftLabel("Integration");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::SliderInt(CONCAT("##mt63_integ_", _this->name), &_this->integ, 8, 64)) {
                _this->decoder->setIntegration(_this->integ);
                _this->saveConfig();
            }
            if (ImGui::Checkbox(CONCAT("8-bit extended chars##mt63_8bit_", _this->name), &_this->eightBit)) {
                _this->decoder->set8bit(_this->eightBit);
                _this->saveConfig();
            }
        }

        // --- Squelch (RF power gate) -----------------------------------------
        if (ImGui::Checkbox(CONCAT("Squelch##mt63_sqen_", _this->name), &_this->squelchEnabled)) {
            _this->decoder->setSquelchEnabled(_this->squelchEnabled);
            _this->saveConfig();
        }
        ImGui::BeginDisabled(!_this->squelchEnabled);
        ImGui::SetNextItemWidth(menuWidth);
        if (ImGui::SliderFloat(CONCAT("##mt63_sql_", _this->name), &_this->squelchLevel,
                               mt63::MT63_SQUELCH_MIN, mt63::MT63_SQUELCH_MAX, "%.1f dB")) {
            _this->decoder->setSquelchLevel(_this->squelchLevel);
            _this->saveConfig();
        }
        ImGui::EndDisabled();

        // --- Status read-outs -------------------------------------------------
        bool lock = _this->decoder->getLock();
        ImU32 lockCol = lock ? IM_COL32(80, 220, 120, 255) : IM_COL32(120, 120, 120, 255);
        ImGui::TextColored(ImColor(lockCol), lock ? "LOCK" : "----");
        ImGui::SameLine();
        ImGui::Text("conf %3.0f%%", _this->decoder->getConfidence() * 100.0f);
        ImGui::SameLine();
        ImGui::Text("  f/o %+5.1f Hz", _this->decoder->getFreqOffset());

        ImGui::Text("S/N: %4.1f dB", _this->decoder->getSNR());
        ImGui::SameLine();
        ImGui::Text("   %d Hz / %s / %.0f sym/s",
                    mt63::MT63_MODES[_this->modeIdx].bw,
                    mt63::MT63_MODES[_this->modeIdx].longIntlv ? "Long" : "Short",
                    _this->decoder->getBaud());

        // --- Decoded text -----------------------------------------------------
        ImGui::TextUnformatted("Decoded text:");
        {
            std::lock_guard<std::mutex> lck(_this->textMtx);
            ImGui::BeginChild(CONCAT("##mt63_txt_", _this->name), ImVec2(menuWidth, 200.0f), true,
                              ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(_this->decodedText.c_str(),
                                   _this->decodedText.c_str() + _this->decodedText.size());
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
                ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();
        }

        if (ImGui::Button(CONCAT("Clear##mt63_clr_", _this->name), ImVec2(menuWidth, 0))) {
            std::lock_guard<std::mutex> lck(_this->textMtx);
            _this->decodedText.clear();
        }

        if (!_this->enabled) { style::endDisabled(); }
    }

    static const char* modesStr() {
        static std::string s;
        if (s.empty()) {
            for (int i = 0; i < mt63::MT63_MODE_COUNT; i++) {
                s += mt63::MT63_MODES[i].name;
                s += '\0';
            }
            s += '\0';
        }
        return s.c_str();
    }

    void saveConfig() {
        config.acquire();
        config.conf[name]["vfoMode"]        = vfoMode;
        config.conf[name]["modeIdx"]        = modeIdx;
        config.conf[name]["snapId"]         = snapId;
        config.conf[name]["centerHz"]       = centerHz;
        config.conf[name]["integ"]          = integ;
        config.conf[name]["eightBit"]       = eightBit;
        config.conf[name]["squelchEnabled"] = squelchEnabled;
        config.conf[name]["squelchLevel"]   = squelchLevel;
        config.release(true);
    }

    std::string name;
    bool enabled = true;

    VFOManager::VFO*    vfo     = nullptr;
    mt63::Mt63Decoder*  decoder = nullptr;

    OptionList<int, double> snapIntervals;

    // Settings
    int    vfoMode        = mt63::VFO_MODE_USB;
    int    modeIdx        = 2;      // MT63-1000S
    int    snapId         = 2;      // 50 Hz
    float  centerHz       = 1500.0f;
    int    integ          = 32;
    bool   eightBit       = false;
    bool   squelchEnabled = false;
    float  squelchLevel   = -50.0f;

    // Decoded text
    std::mutex  textMtx;
    std::string decodedText;
};

MOD_EXPORT void _INIT_() {
    std::string root = (std::string)core::args["root"];
    json def = json({});
    config.setPath(root + "/mt63_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new Mt63DecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (Mt63DecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
