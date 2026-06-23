#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <gui/widgets/folder_select.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <utils/optionlist.h>
#include <utils/flog.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include "decoder.h"
#include "hell/decoder.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#ifdef _WIN32
#include <windows.h>
#include <GL/gl.h>
#else
#include <GL/gl.h>
#endif

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "hell_decoder",
    /* Description:     */ "Hellschreiber decoder (Feld/Slow/X5/X9 + FSK Hell, FLDIGI family)",
    /* Author:          */ "SDR++ community",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

ConfigManager config;

#define VFO_SAMPLE_RATE  (hell::AUDIO_SR)   // 8 kHz, FLDIGI feld native rate
#define VFO_BANDWIDTH    3000.0

class HellDecoderModule : public ModuleManager::Instance {
public:
    HellDecoderModule(std::string name)
        : folderSelect("") {
        this->name = name;

        snapIntervals.define(1,    "1 Hz",    1.0);
        snapIntervals.define(10,   "10 Hz",   10.0);
        snapIntervals.define(100,  "100 Hz",  100.0);
        snapIntervals.define(250,  "250 Hz",  250.0);
        snapIntervals.define(500,  "500 Hz",  500.0);
        snapIntervals.define(1000, "1 kHz",   1000.0);

        agcList.define(hell::HELL_AGC_SLOW,   "Slow",   hell::HELL_AGC_SLOW);
        agcList.define(hell::HELL_AGC_MEDIUM, "Medium", hell::HELL_AGC_MEDIUM);
        agcList.define(hell::HELL_AGC_FAST,   "Fast",   hell::HELL_AGC_FAST);

        formatList.define("PNG", ImageFmt::PNG);
        formatList.define("JPG", ImageFmt::JPG);

        savePath = (std::string)core::args["root"];

        // Load config defensively.
        config.acquire();
        try {
            auto& c = config.conf[name];
            if (c.contains("vfoMode")        && c["vfoMode"].is_number())        { vfoMode        = c["vfoMode"]; }
            if (c.contains("modeIdx")        && c["modeIdx"].is_number())        { modeIdx        = c["modeIdx"]; }
            if (c.contains("snapId")         && c["snapId"].is_number())         { snapId         = c["snapId"]; }
            if (c.contains("afFreq")         && c["afFreq"].is_number())         { afFreq         = c["afFreq"]; }
            if (c.contains("rxHeight")       && c["rxHeight"].is_number())       { rxHeight       = c["rxHeight"]; }
            if (c.contains("agcMode")        && c["agcMode"].is_number())        { agcMode        = c["agcMode"]; }
            if (c.contains("reverse")        && c["reverse"].is_boolean())       { reverse        = c["reverse"]; }
            if (c.contains("blackboard")     && c["blackboard"].is_boolean())    { blackboard     = c["blackboard"]; }
            if (c.contains("squelchEnabled") && c["squelchEnabled"].is_boolean()){ squelchEnabled = c["squelchEnabled"]; }
            if (c.contains("squelchLevel")   && c["squelchLevel"].is_number())   { squelchLevel   = c["squelchLevel"]; }
            if (c.contains("formatId")       && c["formatId"].is_number())       { formatId       = c["formatId"]; }
            if (c.contains("jpegQuality")    && c["jpegQuality"].is_number())    { jpegQuality    = c["jpegQuality"]; }
            if (c.contains("savePath")       && c["savePath"].is_string())       { savePath       = c["savePath"]; }
        }
        catch (...) { /* ignore bad config */ }
        config.release();

        vfoMode  = std::clamp(vfoMode, 0, 2);
        modeIdx  = std::clamp(modeIdx, 0, hell::HELL_MODE_COUNT - 1);
        snapId   = std::clamp(snapId, 0, snapIntervals.size() - 1);
        rxHeight = std::clamp(rxHeight, 14, hell::MAX_RX_COLUMN_LEN);

        folderSelect.setPath(savePath);

        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0,
                                            VFO_BANDWIDTH, VFO_SAMPLE_RATE, 200.0, 15000.0, false);
        if (!vfo) { return; }
        vfo->setSnapInterval(snapIntervals.value(snapId));

        decoder = new hell::HellDecoder(vfo->output);
        decoder->setVFOMode((hell::VfoMode)vfoMode);
        applyVfoGeometry();
        decoder->setMode(modeIdx);
        decoder->setAFFreq(afFreq);
        decoder->setRxHeight(rxHeight);
        decoder->setAgcMode(agcMode);
        decoder->setReverse(reverse);
        decoder->setBlackboard(blackboard);
        decoder->setSquelchEnabled(squelchEnabled);
        decoder->setSquelchLevel(squelchLevel);
        decoder->start();

        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~HellDecoderModule() {
        gui::menu.removeEntry(name);
        if (decoder) { decoder->stop(); delete decoder; decoder = nullptr; }
        if (vfo) { sigpath::vfoManager.deleteVFO(vfo); vfo = nullptr; }
        if (texture) { glDeleteTextures(1, &texture); texture = 0; }
    }

    void postInit() {}

    void enable() {
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0,
                                            VFO_BANDWIDTH, VFO_SAMPLE_RATE, 200.0, 15000.0, false);
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

    // USB -> marker on the lower edge, LSB -> upper edge, NFM -> centered.
    void applyVfoGeometry() {
        if (!vfo) { return; }
        if (vfoMode == hell::VFO_MODE_USB) {
            vfo->setBandwidthLimits(200.0, 15000.0, false);
            vfo->setReference(ImGui::WaterfallVFO::REF_LOWER);
            vfo->setBandwidth(hell::HELL_SSB_BW);
        }
        else if (vfoMode == hell::VFO_MODE_LSB) {
            vfo->setBandwidthLimits(200.0, 15000.0, false);
            vfo->setReference(ImGui::WaterfallVFO::REF_UPPER);
            vfo->setBandwidth(hell::HELL_SSB_BW);
        }
        else { // NFM
            vfo->setBandwidthLimits(2500.0, 15000.0, false);
            vfo->setReference(ImGui::WaterfallVFO::REF_CENTER);
            vfo->setBandwidth(hell::HELL_NFM_BW);
        }
    }

private:
    enum class ImageFmt { PNG, JPG };

    void uploadTexture() {
        if (!decoder) return;
        int w = decoder->getImageWidth();
        int h = decoder->getImageHeight();
        if (w <= 0 || h <= 0) return;

        std::lock_guard<std::mutex> ilck(decoder->getImageMutex());
        const uint8_t* data = decoder->getImageRGB();
        if (!data) return;

        if (texture == 0 || texW != w || texH != h) {
            if (texture != 0) glDeleteTextures(1, &texture);
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
            texW = w; texH = h;
        } else {
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, data);
        }
    }

    void doSaveImage() {
        if (!decoder) return;
        int w = decoder->getImageWidth();
        int h = decoder->getImageHeight();
        if (w <= 0 || h <= 0) return;

        std::vector<uint8_t> snapshot;
        {
            std::lock_guard<std::mutex> ilck(decoder->getImageMutex());
            const uint8_t* data = decoder->getImageRGB();
            if (!data) return;
            snapshot.assign(data, data + (size_t)w * h * 3);
        }

        auto t  = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        std::ostringstream fn;
        fn << "hell_" << std::put_time(&tm, "%Y%m%d_%H%M%S");
        std::filesystem::path base(savePath);
        bool ok = false;
        if (formatList.value(formatId) == ImageFmt::JPG) {
            auto p = (base / (fn.str() + ".jpg")).string();
            ok = stbi_write_jpg(p.c_str(), w, h, 3, snapshot.data(), jpegQuality) != 0;
            if (ok) flog::info("[HELL] saved {0}", p);
        } else {
            auto p = (base / (fn.str() + ".png")).string();
            ok = stbi_write_png(p.c_str(), w, h, 3, snapshot.data(), w * 3) != 0;
            if (ok) flog::info("[HELL] saved {0}", p);
        }
        if (!ok) flog::error("[HELL] image save failed");
    }

    static void menuHandler(void* ctx) {
        HellDecoderModule* _this = (HellDecoderModule*)ctx;
        if (!_this->decoder) { return; }
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) { style::beginDisabled(); }

        // --- Mode -------------------------------------------------------------
        ImGui::LeftLabel("Mode");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(CONCAT("##hell_mode_", _this->name), &_this->modeIdx, modesStr())) {
            _this->modeIdx = std::clamp(_this->modeIdx, 0, hell::HELL_MODE_COUNT - 1);
            _this->decoder->setMode(_this->modeIdx);
            _this->saveConfig();
        }

        // --- VFO mode (sideband) ---------------------------------------------
        ImGui::LeftLabel("VFO mode");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(CONCAT("##hell_vfomode_", _this->name), &_this->vfoMode, "USB\0LSB\0NFM\0")) {
            _this->vfoMode = std::clamp(_this->vfoMode, 0, 2);
            _this->decoder->setVFOMode((hell::VfoMode)_this->vfoMode);
            _this->applyVfoGeometry();
            _this->saveConfig();
        }

        // --- Snap interval ----------------------------------------------------
        ImGui::LeftLabel("Snap");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(CONCAT("##hell_snap_", _this->name), &_this->snapId, _this->snapIntervals.txt)) {
            if (_this->vfo) { _this->vfo->setSnapInterval(_this->snapIntervals.value(_this->snapId)); }
            _this->saveConfig();
        }

        // --- Band view: audio passband spectrum; centre marker = carrier (AF).
        //     Click/drag to set the AF. ---------------------------------------
        {
            static float spec[256];
            int nb = _this->decoder->getBandSpectrum(spec, 256);
            double flo = _this->decoder->getBandFlo();
            double fhi = _this->decoder->getBandFhi();
            double ctr = _this->afFreq;
            double span = hell::HELL_MODES[_this->modeIdx].hell_bandwidth;

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
                float xa = f2x(ctr - span / 2.0), xb = f2x(ctr + span / 2.0);
                dl->AddRectFilled(ImVec2(xa, p0.y), ImVec2(xb, p0.y + h), cBand);
            }
            float xc = f2x(ctr);
            dl->AddLine(ImVec2(xc, p0.y), ImVec2(xc, p0.y + h), cCtr, 2.0f);

            ImGui::InvisibleButton(CONCAT("##hell_band_", _this->name), ImVec2(w, h));
            if (ImGui::IsItemActive()) {
                float mxs = ImGui::GetIO().MousePos.x;
                double f = flo + (double)((mxs - p0.x) / w) * (fhi - flo);
                f = std::round(f);
                if (f < 300) f = 300; if (f > 3300) f = 3300;
                _this->afFreq = (float)f;
                _this->decoder->setAFFreq(_this->afFreq);
            }
            if (ImGui::IsItemDeactivatedAfterEdit() || (ImGui::IsItemHovered() && ImGui::IsMouseReleased(0)))
                _this->saveConfig();
        }

        // --- AF frequency -----------------------------------------------------
        ImGui::LeftLabel("AF freq");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::SliderFloat(CONCAT("##hell_af_", _this->name), &_this->afFreq, 300.0f, 3300.0f, "%.0f Hz")) {
            _this->decoder->setAFFreq(_this->afFreq);
            _this->saveConfig();
        }

        // --- RX height --------------------------------------------------------
        ImGui::LeftLabel("RX height");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::SliderInt(CONCAT("##hell_rxh_", _this->name), &_this->rxHeight, 14, hell::MAX_RX_COLUMN_LEN, "%d px")) {
            _this->rxHeight &= ~1;                       // even (FLDIGI step 2)
            _this->rxHeight = std::clamp(_this->rxHeight, 14, hell::MAX_RX_COLUMN_LEN);
            _this->decoder->setRxHeight(_this->rxHeight);
            _this->decoder->clearImage();
            _this->saveConfig();
        }

        // --- AGC --------------------------------------------------------------
        ImGui::LeftLabel("AGC");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        {
            int sel = _this->agcList.keyId(_this->agcMode);
            if (sel < 0) sel = 0;
            if (ImGui::Combo(CONCAT("##hell_agc_", _this->name), &sel, _this->agcList.txt)) {
                _this->agcMode = _this->agcList.value(sel);
                _this->decoder->setAgcMode(_this->agcMode);
                _this->saveConfig();
            }
        }

        // --- Reverse (FSK only) / Blackboard ---------------------------------
        bool isFsk = hell::HELL_MODES[_this->modeIdx].fsk;
        if (!isFsk) { ImGui::BeginDisabled(); }
        if (ImGui::Checkbox(CONCAT("Reverse##hell_rev_", _this->name), &_this->reverse)) {
            _this->decoder->setReverse(_this->reverse);
            _this->saveConfig();
        }
        if (!isFsk) { ImGui::EndDisabled(); }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("FSK modes only: swaps mark/space to correct an\n"
                              "inverted (wrong-sideband) FSK signal.");
        }
        ImGui::SameLine();
        if (ImGui::Checkbox(CONCAT("Blackboard##hell_bb_", _this->name), &_this->blackboard)) {
            _this->decoder->setBlackboard(_this->blackboard);
            _this->saveConfig();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Invert the displayed image (white-on-black).");
        }

        // --- Squelch ----------------------------------------------------------
        if (ImGui::Checkbox(CONCAT("Squelch##hell_sqen_", _this->name), &_this->squelchEnabled)) {
            _this->decoder->setSquelchEnabled(_this->squelchEnabled);
            _this->saveConfig();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("metric %.0f", _this->decoder->getMetric());
        ImGui::SetNextItemWidth(menuWidth);
        if (ImGui::SliderFloat(CONCAT("##hell_sql_", _this->name), &_this->squelchLevel,
                               hell::HELL_SQUELCH_MIN, hell::HELL_SQUELCH_MAX, "%.1f dB")) {
            _this->decoder->setSquelchLevel(_this->squelchLevel);
            _this->saveConfig();
        }

        // --- Image display ----------------------------------------------------
        ImGui::Separator();
        _this->uploadTexture();
        int cols = _this->decoder->getColumnsReceived();
        if (_this->texture != 0 && cols > 0) {
            float w     = menuWidth;
            int   imgW  = _this->texW;
            int   imgH  = _this->texH;
            float zoom  = 2.0f;                                   // px per Hell pixel
            int   visCols = std::max(1, std::min(imgW, (int)(w / zoom)));
            float u0    = 1.0f - (float)visCols / (float)imgW;    // newest on the right
            float h     = (float)imgH * zoom;
            ImGui::Image((ImTextureID)(uintptr_t)_this->texture, ImVec2(w, h),
                         ImVec2(u0, 0.0f), ImVec2(1.0f, 1.0f));
        } else {
            ImGui::TextDisabled("(no image yet)");
        }

        // --- Controls ---------------------------------------------------------
        ImGui::Separator();
        if (ImGui::Button(CONCAT("Clear##hell_clr_", _this->name), ImVec2(menuWidth, 0))) {
            _this->decoder->clearImage();
        }
        if (ImGui::Button(CONCAT("Save image##hell_save_", _this->name), ImVec2(menuWidth, 0))) {
            _this->doSaveImage();
        }

        ImGui::LeftLabel("Format");
        ImGui::FillWidth();
        if (ImGui::Combo(CONCAT("##hell_fmt_", _this->name), &_this->formatId, _this->formatList.txt)) {
            _this->saveConfig();
        }
        if (_this->formatList.value(_this->formatId) == ImageFmt::JPG) {
            ImGui::LeftLabel("Quality");
            ImGui::FillWidth();
            if (ImGui::SliderInt(CONCAT("##hell_jq_", _this->name), &_this->jpegQuality, 1, 100)) {
                _this->saveConfig();
            }
        }
        ImGui::LeftLabel("Folder");
        ImGui::FillWidth();
        if (_this->folderSelect.render(CONCAT("##hell_folder_", _this->name))) {
            if (_this->folderSelect.pathIsValid()) {
                _this->savePath = _this->folderSelect.path;
                _this->saveConfig();
            }
        }

        if (!_this->enabled) { style::endDisabled(); }
    }

    static const char* modesStr() {
        static std::string s;
        if (s.empty()) {
            for (int i = 0; i < hell::HELL_MODE_COUNT; i++) {
                s += hell::HELL_MODES[i].name;
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
        config.conf[name]["afFreq"]         = afFreq;
        config.conf[name]["rxHeight"]       = rxHeight;
        config.conf[name]["agcMode"]        = agcMode;
        config.conf[name]["reverse"]        = reverse;
        config.conf[name]["blackboard"]     = blackboard;
        config.conf[name]["squelchEnabled"] = squelchEnabled;
        config.conf[name]["squelchLevel"]   = squelchLevel;
        config.conf[name]["formatId"]       = formatId;
        config.conf[name]["jpegQuality"]    = jpegQuality;
        config.conf[name]["savePath"]       = savePath;
        config.release(true);
    }

    std::string name;
    bool enabled = true;

    VFOManager::VFO*   vfo     = nullptr;
    hell::HellDecoder* decoder = nullptr;

    OptionList<int, double>      snapIntervals;
    OptionList<int, int>         agcList;
    OptionList<std::string, ImageFmt> formatList;
    FolderSelect                 folderSelect;

    // GL texture
    GLuint texture = 0;
    int    texW = 0, texH = 0;

    // Settings
    int    vfoMode        = hell::VFO_MODE_USB;
    int    modeIdx        = hell::MODE_FELDHELL;
    int    snapId         = 0;
    float  afFreq         = 1500.0f;
    int    rxHeight       = 20;
    int    agcMode        = hell::HELL_AGC_SLOW;
    bool   reverse        = false;
    bool   blackboard     = false;
    bool   squelchEnabled = true;
    float  squelchLevel   = -50.0f;
    int    formatId       = 0;
    int    jpegQuality    = 90;
    std::string savePath;
};

MOD_EXPORT void _INIT_() {
    std::string root = (std::string)core::args["root"];
    json def = json({});
    config.setPath(root + "/hell_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new HellDecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (HellDecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
