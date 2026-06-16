#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <filesystem>
#include <algorithm>
#include "meteor_demod.h"
#include <dsp/routing/splitter.h>
#include <dsp/buffer/reshaper.h>
#include <dsp/sink/handler_sink.h>
#include <meteor_demodulator_interface.h>
#include <gui/widgets/folder_select.h>
#include <gui/widgets/constellation_diagram.h>
#include <utils/opengl_include_code.h>

#include <fstream>
#include <thread>
#include <atomic>
#include <vector>

#include "lrpt/lrpt_decoder.h"
#include "lrpt/stb_image_write.h"

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "meteor_demodulator",
    /* Description:     */ "Meteor demodulator + LRPT image decoder for SDR++",
    /* Author:          */ "Ryzerth;F4JTV (LRPT decode, ported from SatDump)",
    /* Version:         */ 0, 3, 11,
    /* Max instances    */ -1
};

ConfigManager config;

std::string genFileName(std::string prefix, std::string suffix) {
    time_t now = time(0);
    tm* ltm = localtime(&now);
    char buf[1024];
    sprintf(buf, "%s_%02d-%02d-%02d_%02d-%02d-%02d%s", prefix.c_str(), ltm->tm_hour, ltm->tm_min, ltm->tm_sec, ltm->tm_mday, ltm->tm_mon + 1, ltm->tm_year + 1900, suffix.c_str());
    return buf;
}

#define INPUT_SAMPLE_RATE 150000

// A growable RGBA OpenGL texture, lazily created in the render thread (where the
// GL context is active) and recreated when the image dimensions change. This is
// the directly-managed-texture pattern (avoids ImGui::ImageDisplay's fixed size).
class GrowableTexture {
public:
    ~GrowableTexture() {
        if (textureId) glDeleteTextures(1, &textureId);
    }

    // Upload an RGBA8 buffer (width*height*4). Call from the render thread.
    void update(const uint8_t* rgba, int width, int height) {
        if (!rgba || width <= 0 || height <= 0) return;
        if (!created) {
            glGenTextures(1, &textureId);
            created = true;
        }
        glBindTexture(GL_TEXTURE_2D, textureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        w = width; h = height;
        valid = true;
    }

    bool isValid() const { return valid; }
    GLuint id() const { return textureId; }
    int width() const { return w; }
    int height() const { return h; }

private:
    GLuint textureId = 0;
    bool created = false, valid = false;
    int w = 0, h = 0;
};

class MeteorDemodulatorModule : public ModuleManager::Instance {
public:
    MeteorDemodulatorModule(std::string name) : folderSelect("%ROOT%/recordings"), pngFolderSelect("%ROOT%/recordings") {
        this->name = name;

        writeBuffer = new int8_t[STREAM_BUFFER_SIZE];

        // Load config
        config.acquire();
        if (!config.conf.contains(name)) {
            config.conf[name] = json({});
        }
        if (config.conf[name].contains("recPath")) {
            folderSelect.setPath(config.conf[name]["recPath"]);
        }
        if (config.conf[name].contains("pngPath")) {
            pngFolderSelect.setPath(config.conf[name]["pngPath"]);
        }
        if (config.conf[name].contains("brokenModulation")) {
            brokenModulation = config.conf[name]["brokenModulation"];
        }
        if (config.conf[name].contains("oqpsk")) {
            oqpsk = config.conf[name]["oqpsk"];
        }
        if (config.conf[name].contains("lrptDiff")) {
            lrptDiff = config.conf[name]["lrptDiff"];
        }
        if (config.conf[name].contains("lrptMode")) {
            lrptMode = config.conf[name]["lrptMode"];
        }
        config.release();

        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, INPUT_SAMPLE_RATE, INPUT_SAMPLE_RATE, INPUT_SAMPLE_RATE, INPUT_SAMPLE_RATE, true);
        demod.init(vfo->output, (float)symbolrateForIndex(lrptMode), INPUT_SAMPLE_RATE, 33, 0.6f, 0.1f, 0.005f, brokenModulation, oqpsk, 1e-6, 0.01);
        split.init(&demod.out);
        split.bindStream(&symSinkStream);
        split.bindStream(&sinkStream);
        reshape.init(&symSinkStream, 1024, (72000 / 30) - 1024);
        symSink.init(&reshape.out, symSinkHandler, this);
        sink.init(&sinkStream, sinkHandler, this);

        decoder = std::make_unique<LRPTDecoder>(decoderModeForIndex(lrptMode), lrptDiff);

        // Background image builder (keeps the heavy rebuild off the UI thread).
        imgThreadRun = true;
        imgThread = std::thread(&MeteorDemodulatorModule::imageThreadWorker, this);

        demod.start();
        split.start();
        reshape.start();
        symSink.start();
        sink.start();

        gui::menu.registerEntry(name, menuHandler, this, this);
        core::modComManager.registerInterface("meteor_demodulator", name, moduleInterfaceHandler, this);
    }

    ~MeteorDemodulatorModule() {
        imgThreadRun = false;
        if (imgThread.joinable()) imgThread.join();
        if (recording) {
            std::lock_guard<std::mutex> lck(recMtx);
            recording = false;
            recFile.close();
        }
        stopFileDecode();
        demod.stop();
        split.stop();
        reshape.stop();
        symSink.stop();
        sink.stop();
        sigpath::vfoManager.deleteVFO(vfo);
        gui::menu.removeEntry(name);
        delete[] writeBuffer;
    }

    void postInit() {}

    void enable() {
        double bw = gui::waterfall.getBandwidth();
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, std::clamp<double>(0, -bw / 2.0, bw / 2.0), 150000, INPUT_SAMPLE_RATE, 150000, 150000, true);

        demod.setBrokenModulation(brokenModulation);
        demod.setInput(vfo->output);

        demod.start();
        split.start();
        reshape.start();
        symSink.start();
        sink.start();

        enabled = true;
    }

    void disable() {
        demod.stop();
        split.stop();
        reshape.stop();
        symSink.stop();
        sink.stop();

        sigpath::vfoManager.deleteVFO(vfo);
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    // LRPT mode index -> decoder mode / symbol rate
    static LRPTDecoder::Mode decoderModeForIndex(int idx) {
        if (idx == 1) return LRPTDecoder::MODE_M2X_INT; // M2-4, 80k interleaved
        if (idx == 2) return LRPTDecoder::MODE_LEGACY;  // old M2, 72k QPSK
        return LRPTDecoder::MODE_M2X;                   // M2-3, 72k OQPSK non-interleaved
    }
    static double symbolrateForIndex(int idx) {
        return (idx == 1) ? 80000.0 : 72000.0; // M2-4 is 80k, the rest 72k
    }
    static bool oqpskForIndex(int idx) {
        return (idx == 0 || idx == 1); // M2-3 / M2-4 are OQPSK; legacy is QPSK
    }
    static bool diffForIndex(int idx) {
        return (idx == 0 || idx == 1); // M2-3 / M2-4 use NRZ-M differential; legacy does not
    }

    // Apply a satellite-mode change: decoder pipeline + demod symbol rate + OQPSK + differential.
    void applyLrptMode(int idx) {
        lrptMode = idx;
        // Differential decoding follows the satellite (M2-3/M2-4 need it, legacy doesn't).
        lrptDiff = diffForIndex(idx);
        if (decoder) {
            decoder->setMode(decoderModeForIndex(idx));
            decoder->setDiffDecode(lrptDiff);
        }
        demod.setSymbolrate(symbolrateForIndex(idx));
        bool wantOqpsk = oqpskForIndex(idx);
        if (wantOqpsk != oqpsk) {
            oqpsk = wantOqpsk;
            demod.setOQPSK(oqpsk);
        }
        config.acquire();
        config.conf[name]["lrptMode"] = lrptMode;
        config.conf[name]["oqpsk"] = oqpsk;
        config.conf[name]["lrptDiff"] = lrptDiff;
        config.release(true);
    }

    static void menuHandler(void* ctx) {
        MeteorDemodulatorModule* _this = (MeteorDemodulatorModule*)ctx;

        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) { style::beginDisabled(); }

        ImGui::SetNextItemWidth(menuWidth);
        _this->constDiagram.draw();

        if (_this->folderSelect.render("##meteor_rec" + _this->name)) {
            if (_this->folderSelect.pathIsValid()) {
                config.acquire();
                config.conf[_this->name]["recPath"] = _this->folderSelect.path;
                config.release(true);
            }
        }

        if (ImGui::Checkbox(CONCAT("Broken modulation##meteor_rec", _this->name), &_this->brokenModulation)) {
            _this->demod.setBrokenModulation(_this->brokenModulation);
            config.acquire();
            config.conf[_this->name]["brokenModulation"] = _this->brokenModulation;
            config.release(true);
        }

        if (ImGui::Checkbox(CONCAT("OQPSK##oqpsk", _this->name), &_this->oqpsk)) {
            _this->demod.setOQPSK(_this->oqpsk);
            config.acquire();
            config.conf[_this->name]["oqpsk"] = _this->oqpsk;
            config.release(true);
        }

        if (!_this->folderSelect.pathIsValid() && _this->enabled) { style::beginDisabled(); }

        if (_this->recording) {
            if (ImGui::Button(CONCAT("Stop##meteor_rec_", _this->name), ImVec2(menuWidth, 0))) {
                _this->stopRecording();
            }
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Recording %.2fMB", (float)_this->dataWritten / 1000000.0f);
        }
        else {
            if (ImGui::Button(CONCAT("Record##meteor_rec_", _this->name), ImVec2(menuWidth, 0))) {
                _this->startRecording();
            }
            ImGui::TextUnformatted("Idle --.--MB");
        }

        if (!_this->folderSelect.pathIsValid() && _this->enabled) { style::endDisabled(); }

        if (!_this->enabled) { style::endDisabled(); }

        // ---------------- LRPT image decoder ----------------
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("LRPT image decoder");
        ImGui::Spacing();

        if (ImGui::Checkbox(CONCAT("Decode LRPT (live)##lrpt_live", _this->name), &_this->liveDecode)) {
            // Nothing else needed; sinkHandler checks the flag.
        }

        // Satellite / coding mode
        ImGui::TextUnformatted("Satellite:");
        ImGui::SetNextItemWidth(menuWidth);
        const char* modeItems =
            "Meteor-M2-3 / M2-4 (72k OQPSK)\0"
            "Meteor M2-x (80k interleaved - fallback)\0"
            "Meteor-M2 (legacy, 72k QPSK)\0";
        if (ImGui::Combo(CONCAT("##lrpt_mode", _this->name), &_this->lrptMode, modeItems)) {
            _this->applyLrptMode(_this->lrptMode);
        }
        if (_this->lrptMode == 0)
            ImGui::TextDisabled("137.9 MHz (M2-4 backup 137.1). Try this mode first.");
        else if (_this->lrptMode == 1)
            ImGui::TextDisabled("80k interleaved - only if 72k does not sync");

        if (ImGui::Checkbox(CONCAT("Differential decode##lrpt_diff", _this->name), &_this->lrptDiff)) {
            if (_this->decoder) _this->decoder->setDiffDecode(_this->lrptDiff);
            config.acquire();
            config.conf[_this->name]["lrptDiff"] = _this->lrptDiff;
            config.release(true);
        }

        if (ImGui::Checkbox(CONCAT("Ignore RS errors (rs_usecheck off)##lrpt_rsbypass", _this->name), &_this->lrptRsBypass)) {
            if (_this->decoder) _this->decoder->setRsBypass(_this->lrptRsBypass);
        }

        // Status / diagnostics
        {
            bool locked = _this->decoder ? _this->decoder->isLocked() : false;
            float ber = _this->decoder ? _this->decoder->getBER() : 0.0f;
            uint64_t frames = _this->decoder ? _this->decoder->getFrames() : 0;
            uint64_t cadus = _this->decoder ? _this->decoder->getCADUs() : 0;
            uint64_t pkts = _this->decoder ? _this->decoder->getPackets() : 0;

            // Track CADU flow over ~1.5 s: if CADUs keep arriving we ARE decoding,
            // regardless of the Viterbi's instantaneous lock state (which flickers).
            double now = ImGui::GetTime();
            if (now - _this->caduSnapTime > 1.5) {
                _this->caduFlowing = (cadus > _this->caduSnapCount);
                _this->caduSnapCount = cadus;
                _this->caduSnapTime = now;
            }

            ImGui::TextUnformatted("State:");
            ImGui::SameLine();
            if (_this->caduFlowing)
                ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "DECODING");
            else if (locked)
                ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.2f, 1.0f), "sync (no CADU)");
            else
                ImGui::TextColored(ImVec4(0.8f, 0.3f, 0.3f, 1.0f), "searching");
            ImGui::SameLine();
            ImGui::Text("| BER %.3f", ber);
            ImGui::Text("Frames: %llu  CADUs: %llu  pkts: %llu",
                        (unsigned long long)frames, (unsigned long long)cadus, (unsigned long long)pkts);

            // Per-channel packet counts (tells us which channels carry image data)
            if (_this->decoder) {
                uint64_t ap[7]; _this->decoder->getApidPackets(ap);
                ImGui::Text("ch pkts: 1:%llu 2:%llu 3:%llu 4:%llu 5:%llu 6:%llu",
                            (unsigned long long)ap[0], (unsigned long long)ap[1], (unsigned long long)ap[2],
                            (unsigned long long)ap[3], (unsigned long long)ap[4], (unsigned long long)ap[5]);
            }
        }

        if (_this->fileDecoding) {
            ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "Decoding file... %d%%", (int)(_this->fileProgress * 100.0f));
        }

        // View selector
        ImGui::SetNextItemWidth(menuWidth);
        const char* viewItems = "Composite RGB\0Channel 1 (APID 64)\0Channel 2 (APID 65)\0Channel 3 (APID 66)\0Channel 4 (APID 67)\0Channel 5 (APID 68)\0Channel 6 (APID 69)\0";
        if (ImGui::Combo(CONCAT("##lrpt_view", _this->name), &_this->viewMode, viewItems)) _this->imgRequestRebuild = true;

        if (ImGui::Checkbox(CONCAT("Normalize contrast##lrpt_norm", _this->name), &_this->normalizeImg))
            _this->imgRequestRebuild = true;
        ImGui::SameLine();
        if (ImGui::Checkbox(CONCAT("Flip 180 (descending pass)##lrpt_flip", _this->name), &_this->flip180))
            _this->imgRequestRebuild = true;

        if (_this->viewMode == 0) {
            ImGui::Checkbox(CONCAT("Auto channels##lrpt_autoch", _this->name), &_this->autoChannels);
            if (_this->autoChannels) {
                int r = 0, g = 0, b = 0; _this->topChannelsByPackets(r, g, b);
                ImGui::TextDisabled("R/G/B = ch %d / %d / %d (auto)", r + 1, g + 1, b + 1);
            } else {
                ImGui::Text("R/G/B channels (0-5):");
                ImGui::SetNextItemWidth(menuWidth / 3.0f - 4);
                ImGui::InputInt(CONCAT("##lrpt_r", _this->name), &_this->compR, 0, 0);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(menuWidth / 3.0f - 4);
                ImGui::InputInt(CONCAT("##lrpt_g", _this->name), &_this->compG, 0, 0);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(menuWidth / 3.0f - 4);
                ImGui::InputInt(CONCAT("##lrpt_b", _this->name), &_this->compB, 0, 0);
                _this->compR = std::clamp(_this->compR, 0, 5);
                _this->compG = std::clamp(_this->compG, 0, 5);
                _this->compB = std::clamp(_this->compB, 0, 5);
            }
        }

        if (ImGui::Button(CONCAT("Refresh image##lrpt_refresh", _this->name), ImVec2(menuWidth, 0))) {
            _this->imgRequestRebuild = true; // handled by the background image thread
        }

        // Image display (the background thread keeps displayRGBA up to date)
        int dW = 0, dH = 0;
        bool haveImg = false;
        {
            std::lock_guard<std::mutex> lk(_this->imgMtx);
            haveImg = !_this->displayRGBA.empty() && _this->dispW > 0 && _this->dispH > 0;
            if (haveImg && _this->texDirty) {
                _this->texture.update(_this->displayRGBA.data(), _this->dispW, _this->dispH);
                _this->texDirty = false;
            }
            dW = _this->dispW; dH = _this->dispH;
        }
        // Only draw the image when we have valid, non-zero dimensions. After an
        // auto-save+reset the buffer is cleared (dW=dH=0); drawing then would give
        // an aspect of 0/0 = NaN and corrupt the whole left-menu layout.
        if (haveImg && dW > 0 && dH > 0 && _this->texture.isValid()) {
            float aspect = (float)dH / (float)dW;
            float dw = menuWidth;
            float dh = dw * aspect;
            ImGui::Image((ImTextureID)(intptr_t)_this->texture.id(), ImVec2(dw, dh));
            ImGui::Text("Image: %d x %d", dW, dH);
        }
        else {
            ImGui::TextDisabled("No image yet");
        }

        // PNG output folder (with the "..." file-explorer button) + save.
        ImGui::TextUnformatted("Save folder:");
        if (_this->pngFolderSelect.render("##meteor_png" + _this->name)) {
            if (_this->pngFolderSelect.pathIsValid()) {
                config.acquire();
                config.conf[_this->name]["pngPath"] = _this->pngFolderSelect.path;
                config.release(true);
            }
        }

        if (ImGui::Button(CONCAT("Save PNG##lrpt_save", _this->name), ImVec2(menuWidth, 0))) {
            _this->savePNG();
        }

        ImGui::Checkbox(CONCAT("Auto-save PNG + reset between passes##lrpt_autosave", _this->name), &_this->autoSaveReset);
        if (_this->autoSaveReset) {
            ImGui::SetNextItemWidth(menuWidth * 0.5f);
            if (ImGui::InputInt(CONCAT("LOS gap (s)##lrpt_resetgap", _this->name), &_this->autoResetGapSec, 1, 5))
                _this->autoResetGapSec = std::clamp(_this->autoResetGapSec, 3, 120);
        }

        {
            std::string saved;
            { std::lock_guard<std::mutex> lk(_this->imgMtx); saved = _this->lastSavedPng; }
            if (!saved.empty()) ImGui::TextWrapped("Saved: %s", saved.c_str());
        }

        if (ImGui::Button(CONCAT("Reset decoder##lrpt_reset", _this->name), ImVec2(menuWidth, 0))) {
            if (_this->decoder) _this->decoder->reset();
            std::lock_guard<std::mutex> lk(_this->imgMtx);
            _this->displayRGBA.clear();
            _this->dispW = _this->dispH = 0;
            _this->texDirty = true;
        }

        ImGui::Spacing();
        ImGui::TextUnformatted("Offline decode (.s soft symbols):");
        ImGui::SetNextItemWidth(menuWidth);
        ImGui::InputText(CONCAT("##lrpt_file", _this->name), _this->filePath, sizeof(_this->filePath));
        if (_this->fileDecoding) { style::beginDisabled(); }
        if (ImGui::Button(CONCAT("Decode file##lrpt_decfile", _this->name), ImVec2(menuWidth, 0))) {
            _this->startFileDecode(_this->filePath);
        }
        if (_this->fileDecoding) { style::endDisabled(); }
    }

    static void symSinkHandler(dsp::complex_t* data, int count, void* ctx) {
        MeteorDemodulatorModule* _this = (MeteorDemodulatorModule*)ctx;

        dsp::complex_t* buf = _this->constDiagram.acquireBuffer();
        memcpy(buf, data, 1024 * sizeof(dsp::complex_t));
        _this->constDiagram.releaseBuffer();
    }

    static void sinkHandler(dsp::complex_t* data, int count, void* ctx) {
        MeteorDemodulatorModule* _this = (MeteorDemodulatorModule*)ctx;

        // Convert soft QPSK symbols to int8 I/Q (same scaling as the .s recorder)
        if (_this->recording || _this->liveDecode) {
            for (int i = 0; i < count; i++) {
                _this->writeBuffer[(2 * i)] = std::clamp<int>(data[i].re * 84.0f, -127, 127);
                _this->writeBuffer[(2 * i) + 1] = std::clamp<int>(data[i].im * 84.0f, -127, 127);
            }
        }

        if (_this->liveDecode && _this->decoder) {
            _this->decoder->pushSymbols(_this->writeBuffer, count * 2);
        }

        {
            std::lock_guard<std::mutex> lck(_this->recMtx);
            if (!_this->recording) { return; }
            _this->recFile.write((char*)_this->writeBuffer, count * 2);
            _this->dataWritten += count * 2;
        }
    }

    // Pick the 3 channels carrying the most packets (robust to whichever
    // channels the satellite actually transmits, e.g. M2-3 uses APID 64/65/67).
    void topChannelsByPackets(int& r, int& g, int& b) {
        uint64_t ap[7] = {0};
        if (decoder) decoder->getApidPackets(ap);
        int order[6] = {0, 1, 2, 3, 4, 5};
        std::sort(order, order + 6, [&](int a, int c) { return ap[a] > ap[c]; });
        r = order[0]; g = order[1]; b = order[2];
        // if fewer than 3 active, repeat the strongest
        if (ap[g] == 0) g = r;
        if (ap[b] == 0) b = g;
    }

    // Stretch a grayscale plane's nonzero range to 0..255 (reveals dark IR/visible bands).
    static void normalizePlane(const meteorimg::SimpleImage& img, std::vector<uint8_t>& out, bool normalize) {
        size_t n = img.size();
        out.assign(n, 0);
        if (n == 0) return;
        if (!normalize) { for (size_t i = 0; i < n; i++) out[i] = img.get(i); return; }
        int lo = 255, hi = 0;
        for (size_t i = 0; i < n; i++) { int v = img.get(i); if (v == 0) continue; if (v < lo) lo = v; if (v > hi) hi = v; }
        if (hi <= lo) { for (size_t i = 0; i < n; i++) out[i] = img.get(i); return; }
        float scale = 255.0f / (float)(hi - lo);
        for (size_t i = 0; i < n; i++) {
            int v = img.get(i);
            if (v == 0) { out[i] = 0; continue; } // keep missing-segment gaps black
            int s = (int)((v - lo) * scale + 0.5f);
            out[i] = (uint8_t)std::clamp(s, 0, 255);
        }
    }

    // Per-satellite, per-channel X/Y alignment offset, ported from SatDump's
    // meteor::msumr::get_x_offset(). Channel 4 (index 3, APID 67) needs a small
    // shift on M2-3 / M2-4 for correct composite registration.
    void channelOffset(int ch, int& offX, int& offY) {
        offX = 0; offY = 0;
        if (ch == 3) {
            if (lrptMode == 0) { offX = -2; offY = -2; }      // M2-3 : (-1.6,-1.6) rounded
            else if (lrptMode == 1) { offX = -2; offY = 0; }  // M2-4 : (-2, 0)
        }
    }

    // Builds the display image. Heavy work (channel rebuild + compose) is done
    // into a local buffer; only the final swap touches shared state under imgMtx.
    // Safe to call from the background image thread.
    // Background thread: rebuilds the display image at most every few seconds,
    // and only when new CADUs have arrived (or on explicit request). This keeps
    // the expensive full-image rebuild off the SDR++ UI/render thread, which was
    // causing the waterfall/UI to stutter on long sessions.
    void imageThreadWorker() {
        using namespace std::chrono;
        auto lastBuild = steady_clock::now();
        auto lastChange = steady_clock::now();
        uint64_t lastCadu = 0;
        bool passHadData = false;
        while (imgThreadRun.load()) {
            std::this_thread::sleep_for(milliseconds(200));
            if (!imgThreadRun.load()) break;
            bool req = imgRequestRebuild.exchange(false);
            uint64_t cadu = decoder ? decoder->getCADUs() : 0;
            bool newData = (cadu != lastCadu);
            if (newData) { lastCadu = cadu; lastChange = steady_clock::now(); passHadData = true; }

            // End-of-pass detection: CADUs stopped for a while after a pass had data.
            bool gap = duration_cast<milliseconds>(steady_clock::now() - lastChange).count() > (long)(autoResetGapSec * 1000);
            bool haveImg;
            { std::lock_guard<std::mutex> lk(imgMtx); haveImg = !displayRGBA.empty() && dispW > 0 && dispH > 0; }
            if (autoSaveReset && passHadData && !newData && gap && haveImg) {
                rebuildImage();      // make sure the final image is current
                savePNG();           // auto-save this pass
                decoder->reset();    // start fresh for the next pass
                { std::lock_guard<std::mutex> lk(imgMtx); displayRGBA.clear(); dispW = dispH = 0; texDirty = true; }
                passHadData = false; lastCadu = 0;
                continue;
            }

            // Throttled rebuild (only when new data, or explicit request).
            bool due = duration_cast<milliseconds>(steady_clock::now() - lastBuild).count() > 3000;
            if (req || (liveDecode && newData && due)) {
                lastBuild = steady_clock::now();
                rebuildImage();
            }
        }
    }

    void rebuildImage() {
        if (!decoder) return;
        std::vector<uint8_t> out;
        int ow = 0, oh = 0;

        if (viewMode == 0) {
            int r = compR, g = compG, b = compB;
            if (autoChannels) topChannelsByPackets(r, g, b);
            meteorimg::SimpleImage ir = decoder->getChannelImage(r);
            meteorimg::SimpleImage ig = decoder->getChannelImage(g);
            meteorimg::SimpleImage ib = decoder->getChannelImage(b);
            size_t w = 0, h = 0;
            for (auto* im : {&ir, &ig, &ib}) { if (im->width() > w) w = im->width(); if (im->height() > h) h = im->height(); }
            if (w == 0 || h == 0) return;
            std::vector<uint8_t> pr, pg, pb;
            normalizePlane(ir, pr, normalizeImg);
            normalizePlane(ig, pg, normalizeImg);
            normalizePlane(ib, pb, normalizeImg);
            int orx, ory, ogx, ogy, obx, oby;
            channelOffset(r, orx, ory); channelOffset(g, ogx, ogy); channelOffset(b, obx, oby);
            out.assign(w * h * 4, 255);
            auto sample = [](std::vector<uint8_t>& p, meteorimg::SimpleImage& im, int x, int y, int ox, int oy) -> uint8_t {
                int sx = x + ox, sy = y + oy;
                if (sx < 0 || sy < 0 || sx >= (int)im.width() || sy >= (int)im.height()) return 0;
                size_t idx = (size_t)sy * im.width() + sx;
                return idx < p.size() ? p[idx] : 0;
            };
            for (int y = 0; y < (int)h; y++) {
                for (int x = 0; x < (int)w; x++) {
                    // 180° flip: descending passes arrive upside-down
                    size_t i = flip180 ? ((size_t)(h - 1 - y) * w + (w - 1 - x)) : ((size_t)y * w + x);
                    out[i * 4 + 0] = sample(pr, ir, x, y, orx, ory);
                    out[i * 4 + 1] = sample(pg, ig, x, y, ogx, ogy);
                    out[i * 4 + 2] = sample(pb, ib, x, y, obx, oby);
                    out[i * 4 + 3] = 255;
                }
            }
            ow = (int)w; oh = (int)h;
        }
        else {
            int ch = viewMode - 1;
            meteorimg::SimpleImage img = decoder->getChannelImage(ch);
            if (img.size() == 0) return;
            std::vector<uint8_t> p;
            normalizePlane(img, p, normalizeImg);
            int w = (int)img.width(), h = (int)img.height();
            out.assign((size_t)w * h * 4, 255);
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    uint8_t v = p[(size_t)y * w + x];
                    size_t i = flip180 ? ((size_t)(h - 1 - y) * w + (w - 1 - x)) : ((size_t)y * w + x);
                    out[i * 4 + 0] = v; out[i * 4 + 1] = v; out[i * 4 + 2] = v; out[i * 4 + 3] = 255;
                }
            }
            ow = w; oh = h;
        }

        // Publish (cheap) under the image mutex.
        std::lock_guard<std::mutex> lk(imgMtx);
        displayRGBA.swap(out);
        dispW = ow; dispH = oh; texDirty = true;
    }

    void savePNG() {
        // Snapshot the current display buffer (kept normalized/up-to-date by the
        // image thread) so the saved PNG matches exactly what's shown.
        std::vector<uint8_t> snap;
        int w, h;
        {
            std::lock_guard<std::mutex> lk(imgMtx);
            snap = displayRGBA;
            w = dispW; h = dispH;
        }
        if (snap.empty() || w <= 0 || h <= 0) {
            flog::warn("LRPT: nothing to save yet");
            { std::lock_guard<std::mutex> lk(imgMtx); lastSavedPng = "(no image to save yet)"; }
            return;
        }
        std::string base = pngFolderSelect.pathIsValid() ? pngFolderSelect.expandString(pngFolderSelect.path) : ((std::string)core::args["root"] + "/recordings");
        std::string suffix = (viewMode == 0) ? "_LRPT_RGB" : ("_LRPT_ch" + std::to_string(viewMode));
        std::string filename = genFileName(base + "/meteor", suffix + ".png");
        // Write RGB (drop alpha) for a compact image
        std::vector<uint8_t> rgb((size_t)w * h * 3);
        for (size_t i = 0; i < (size_t)w * h; i++) {
            rgb[i * 3 + 0] = snap[i * 4 + 0];
            rgb[i * 3 + 1] = snap[i * 4 + 1];
            rgb[i * 3 + 2] = snap[i * 4 + 2];
        }
        if (stbi_write_png(filename.c_str(), w, h, 3, rgb.data(), w * 3)) {
            flog::info("LRPT: saved '{0}'", filename);
            { std::lock_guard<std::mutex> lk(imgMtx); lastSavedPng = filename; }
        }
        else {
            flog::error("LRPT: failed to save PNG");
            { std::lock_guard<std::mutex> lk(imgMtx); lastSavedPng = "(save failed - check the folder is writable)"; }
        }
    }

    void startFileDecode(const std::string& path) {
        if (fileDecoding) return;
        if (path.empty() || !std::filesystem::exists(path)) {
            flog::error("LRPT: file does not exist: '{0}'", path);
            return;
        }
        stopFileDecode();
        if (decoder) decoder->reset();
        fileProgress = 0.0f;
        fileDecoding = true;
        fileDecodeStop = false;
        fileThread = std::thread(&MeteorDemodulatorModule::fileDecodeWorker, this, path);
    }

    void stopFileDecode() {
        fileDecodeStop = true;
        if (fileThread.joinable()) fileThread.join();
        fileDecoding = false;
    }

    void fileDecodeWorker(std::string path) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f.is_open()) { fileDecoding = false; return; }
        std::streamsize total = f.tellg();
        f.seekg(0, std::ios::beg);
        const size_t CHUNK = 65536; // int8 samples per read
        std::vector<int8_t> buf(CHUNK);
        std::streamsize done = 0;
        while (!fileDecodeStop && f.good()) {
            f.read((char*)buf.data(), CHUNK);
            std::streamsize got = f.gcount();
            if (got <= 0) break;
            if (decoder) decoder->pushSymbols(buf.data(), (int)got);
            done += got;
            fileProgress = total > 0 ? (float)((double)done / (double)total) : 0.0f;
        }
        fileProgress = 1.0f;
        // Build the final image (on the worker; texture upload happens in render thread)
        rebuildImage();
        fileDecoding = false;
    }

    void startRecording() {
        std::lock_guard<std::mutex> lck(recMtx);
        dataWritten = 0;
        std::string filename = genFileName(folderSelect.expandString(folderSelect.path) + "/meteor", ".s");
        recFile = std::ofstream(filename, std::ios::binary);
        if (recFile.is_open()) {
            flog::info("Recording to '{0}'", filename);
            recording = true;
        }
        else {
            flog::error("Could not open file for recording!");
        }
    }

    void stopRecording() {
        std::lock_guard<std::mutex> lck(recMtx);
        recording = false;
        recFile.close();
        dataWritten = 0;
    }

    static void moduleInterfaceHandler(int code, void* in, void* out, void* ctx) {
        MeteorDemodulatorModule* _this = (MeteorDemodulatorModule*)ctx;
        if (code == METEOR_DEMODULATOR_IFACE_CMD_START) {
            if (!_this->recording) { _this->startRecording(); }
        }
        else if (code == METEOR_DEMODULATOR_IFACE_CMD_STOP) {
            if (_this->recording) { _this->stopRecording(); }
        }
    }

    std::string name;
    bool enabled = true;

    // DSP Chain
    VFOManager::VFO* vfo;
    dsp::demod::Meteor demod;
    dsp::routing::Splitter<dsp::complex_t> split;

    dsp::stream<dsp::complex_t> symSinkStream;
    dsp::stream<dsp::complex_t> sinkStream;
    dsp::buffer::Reshaper<dsp::complex_t> reshape;
    dsp::sink::Handler<dsp::complex_t> symSink;
    dsp::sink::Handler<dsp::complex_t> sink;

    ImGui::ConstellationDiagram constDiagram;

    FolderSelect folderSelect;
    FolderSelect pngFolderSelect;
    std::string lastSavedPng;

    std::mutex recMtx;
    bool recording = false;
    uint64_t dataWritten = 0;
    std::ofstream recFile;
    bool brokenModulation = false;
    bool oqpsk = false;
    int8_t* writeBuffer;

    // LRPT decode
    std::unique_ptr<LRPTDecoder> decoder;
    bool liveDecode = false;
    bool lrptDiff = true;
    uint64_t caduSnapCount = 0;
    double caduSnapTime = 0.0;
    bool caduFlowing = false;
    bool lrptRsBypass = false;
    int  lrptMode = 0; // 0 = M2-3 (72k OQPSK), 1 = M2-4 (80k interleaved), 2 = legacy M2 (72k QPSK)

    int viewMode = 0; // 0 = composite, 1..6 = channels
    int compR = 0, compG = 1, compB = 3; // M2-3 currently transmits APID 64/65/67 = ch 0/1/3
    bool autoChannels = true;
    bool normalizeImg = true;

    std::vector<uint8_t> displayRGBA;
    int dispW = 0, dispH = 0;
    bool texDirty = false;
    std::thread imgThread;
    std::atomic<bool> imgThreadRun{false};
    std::atomic<bool> imgRequestRebuild{false};
    std::mutex imgMtx;
    bool flip180 = false;
    bool autoSaveReset = false;
    int  autoResetGapSec = 30; // LOS gap (s) that triggers auto-save + reset
    GrowableTexture texture;
    double lastRefresh = 0.0;

    char filePath[1024] = "";
    std::thread fileThread;
    std::atomic<bool> fileDecoding{false};
    std::atomic<bool> fileDecodeStop{false};
    std::atomic<float> fileProgress{0.0f};
};

MOD_EXPORT void _INIT_() {
    std::string root = (std::string)core::args["root"];
    if (!std::filesystem::exists(root + "/recordings")) {
        flog::warn("Recordings directory does not exist, creating it");
        if (!std::filesystem::create_directory(root + "/recordings")) {
            flog::error("Could not create recordings directory");
        }
    }
    json def = json({});
    config.setPath(root + "/meteor_demodulator_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new MeteorDemodulatorModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (MeteorDemodulatorModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
