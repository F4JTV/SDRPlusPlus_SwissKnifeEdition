#pragma once
#include "../decoder.h"
#include <signal_path/vfo_manager.h>
#include <utils/flog.h>
#include <gui/style.h>
#include <dsp/sink/handler_sink.h>
#include <imgui.h>
#include <functional>
#include "dsp.h"
#include "flex.h"

// VFO setup for FLEX. FLEX channels are 25 kHz wide in the standard
// 929-932 MHz US band and the 138-174 MHz / 410-470 MHz bands.
// We use 22050 sps internally to match multimon-ng's reference rate
// (the symbol-detector constants are tuned around this value).
#define FLEX_VFO_SAMPLERATE   22050.0
#define FLEX_VFO_BANDWIDTH    15000.0

class FLEXDecoder : public Decoder {
public:
    using MessageCallback = std::function<void(const flex::Message&)>;

    FLEXDecoder(const std::string& name, VFOManager::VFO* vfo, MessageCallback onMessage)
        : decoder(FLEX_VFO_SAMPLERATE)
    {
        this->name        = name;
        this->vfo         = vfo;
        this->onMessageCb = std::move(onMessage);

        // Configure the VFO
        vfo->setBandwidthLimits(FLEX_VFO_BANDWIDTH, FLEX_VFO_BANDWIDTH, true);
        vfo->setSampleRate(FLEX_VFO_SAMPLERATE, FLEX_VFO_BANDWIDTH);

        // DSP chain
        dsp.init(vfo->output, FLEX_VFO_SAMPLERATE);
        dataHandler.init(&dsp.out, _dataHandler, this);

        // Wire the protocol decoder's message event to our static handler
        decoder.onMessage.bind(&FLEXDecoder::messageHandler, this);
    }

    ~FLEXDecoder() override { stop(); }

    void showMenu() override {
        ImGui::TextDisabled("Auto-detects 1600/3200/6400 baud,");
        ImGui::TextDisabled("2-FSK or 4-FSK, both polarities.");

        // Live status line
        if (decoder.isLocked()) {
            unsigned int baud   = decoder.currentSyncBaud();
            unsigned int levels = decoder.currentSyncLevels();
            if (baud > 0 && levels > 0) {
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f),
                                   "Locked: %u bps / %u-FSK", baud, levels);
            } else {
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Locked");
            }
        } else {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Searching sync...");
        }
    }

    void setVFO(VFOManager::VFO* vfo) override {
        this->vfo = vfo;
        vfo->setBandwidthLimits(FLEX_VFO_BANDWIDTH, FLEX_VFO_BANDWIDTH, true);
        vfo->setSampleRate(FLEX_VFO_SAMPLERATE, FLEX_VFO_BANDWIDTH);
        dsp.setInput(vfo->output);
    }

    void start() override {
        dsp.start();
        dataHandler.start();
    }

    void stop() override {
        dsp.stop();
        dataHandler.stop();
    }

private:
    static void _dataHandler(float* data, int count, void* ctx) {
        FLEXDecoder* _this = (FLEXDecoder*)ctx;
        _this->decoder.process(data, count);
    }

    void messageHandler(const flex::Message& m) {
        char capbuf[24];
        std::snprintf(capbuf, sizeof(capbuf), "%lld", (long long)m.capcode);
        flog::info("FLEX: cap={} type={} baud={}/{}/{} {}.{} '{}'",
                   capbuf,
                   (int)m.type,
                   m.baud,
                   m.levels,
                   m.phase,
                   m.cycle,
                   m.frame,
                   m.content);
        if (onMessageCb) { onMessageCb(m); }
    }

    std::string                       name;
    VFOManager::VFO*                  vfo = nullptr;

    FLEXDSP                           dsp;
    dsp::sink::Handler<float>         dataHandler;
    flex::Decoder                     decoder;

    MessageCallback                   onMessageCb;
};
