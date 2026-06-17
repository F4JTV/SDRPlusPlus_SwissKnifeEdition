#pragma once
#include "../decoder.h"
#include <signal_path/vfo_manager.h>
#include <utils/optionlist.h>
#include <utils/flog.h>
#include <gui/widgets/symbol_diagram.h>
#include <gui/style.h>
#include <dsp/buffer/reshaper.h>
#include <dsp/sink/handler_sink.h>
#include <dsp/routing/splitter.h>
#include <imgui.h>
#include <atomic>
#include <functional>
#include <cstring>
#include <ctime>
#include <algorithm>
#include "dsp.h"
#include "pocsag.h"

// Fixed VFO output sample rate. 24 kHz covers the +/-4.5 kHz deviation
// plus the highest POCSAG symbol rate (2400 baud), with sensible samples-
// per-symbol at all three rates:
//    24000 / 512  = ~46.9 sps
//    24000 / 1200 =   20  sps
//    24000 / 2400 =   10  sps
#define POCSAG_VFO_SAMPLERATE   24000.0
#define POCSAG_VFO_BANDWIDTH    12500.0

// Number of symbols shown in the diagram and Reshaper output block
#define POCSAG_DIAG_BLOCK       1024

class POCSAGDecoder : public Decoder {
public:
    using MessageCallback = std::function<void(const pocsag::Message&)>;

    // Three parallel decoder chains, one per POCSAG baudrate. They are
    // always running; in Auto mode any clean (error-free) decode from any
    // chain is surfaced, in Manual mode only the selected chain's output
    // is surfaced. CPU cost is ~3x a single chain, which is negligible
    // for a typical POCSAG channel (the matched filter is ~50 taps).
    static constexpr int  CHAIN_COUNT             = 3;
    static constexpr int  CHAIN_BAUDS[CHAIN_COUNT] = {512, 1200, 2400};

    POCSAGDecoder(const std::string& name, VFOManager::VFO* vfo, MessageCallback onMessage)
        : diag(0.6f, POCSAG_DIAG_BLOCK)
    {
        this->name        = name;
        this->vfo         = vfo;
        this->onMessageCb = std::move(onMessage);

        // Define baudrate options. Sentinel key 0 means "Auto".
        baudrates.define(0,    "Auto",      0);
        baudrates.define(512,  "512 Baud",  512);
        baudrates.define(1200, "1200 Baud", 1200);
        baudrates.define(2400, "2400 Baud", 2400);

        decodeModes.define(pocsag::DECODE_MODE_AUTO,    "Auto",          pocsag::DECODE_MODE_AUTO);
        decodeModes.define(pocsag::DECODE_MODE_ALPHA,   "Alphanumeric",  pocsag::DECODE_MODE_ALPHA);
        decodeModes.define(pocsag::DECODE_MODE_NUMERIC, "Numeric",       pocsag::DECODE_MODE_NUMERIC);

        // Default: Auto baudrate (parallel detection)
        brId   = baudrates.keyId(0);
        dmId   = decodeModes.keyId(pocsag::DECODE_MODE_AUTO);
        invert = false;
        primaryChainIdx.store(1);   // 1200 baud chain's symbol diagram by default

        // VFO setup
        vfo->setBandwidthLimits(POCSAG_VFO_BANDWIDTH, POCSAG_VFO_BANDWIDTH, true);
        vfo->setSampleRate(POCSAG_VFO_SAMPLERATE, POCSAG_VFO_BANDWIDTH);

        // Splitter fans out the VFO IQ to each chain's input stream
        splitter.init(vfo->output);

        // Reshape stride for the symbol diagram
        int reshapeStride = std::max(8, POCSAG_DIAG_BLOCK / 12);

        // Bring up each parallel chain
        for (int i = 0; i < CHAIN_COUNT; i++) {
            Chain& c = chains[i];
            c.baud   = CHAIN_BAUDS[i];
            c.index  = i;
            c.parent = this;

            c.dsp.init(&c.inputStream,
                       POCSAG_VFO_SAMPLERATE,
                       (double)c.baud);

            c.reshape.init(&c.dsp.soft, POCSAG_DIAG_BLOCK,
                           reshapeStride - POCSAG_DIAG_BLOCK);

            c.dataHandler.init(&c.dsp.out,    _chainDataHandler, &c);
            c.diagHandler.init(&c.reshape.out, _chainDiagHandler, &c);

            // Apply initial decoder/dsp settings (same on every chain)
            c.decoder.setDecodeMode(decodeModes.value(dmId));
            c.decoder.setInverted(invert);
            c.dsp.setLowPass(lowPass);

            // Bind the chain's message event to our chain-aware handler
            int chainIdx = i;
            c.decoder.onMessage.bind(
                [this, chainIdx](const pocsag::Message& m) {
                    this->onChainMessage(chainIdx, m);
                });

            // Plug this chain's input stream into the splitter
            splitter.bindStream(&c.inputStream);
        }
    }

    ~POCSAGDecoder() override {
        stop();
        for (auto& c : chains) {
            splitter.unbindStream(&c.inputStream);
        }
    }

    void showMenu() override {
        // Baudrate selector
        ImGui::LeftLabel("Baudrate");
        ImGui::FillWidth();
        if (ImGui::Combo(("##pager_decoder_pocsag_br_" + name).c_str(),
                         &brId, baudrates.txt))
        {
            int chosen = baudrates.value(brId);
            if (chosen != 0) {
                // Manual: point the symbol diagram at the selected chain
                for (int i = 0; i < CHAIN_COUNT; i++) {
                    if (chains[i].baud == chosen) {
                        primaryChainIdx.store(i);
                        break;
                    }
                }
            } else {
                // Auto: reset per-chain counters and let the next decode
                // pick the primary
                for (auto& c : chains) {
                    c.messageCount     = 0;
                    c.lastMessageTime  = 0;
                }
                primaryChainIdx.store(1);
            }
            if (settingsCallback) { settingsCallback(); }
        }

        // Live status: in Auto mode show all three chains' decode counts.
        // The chain currently producing decodes is highlighted in green.
        if (isAutoMode()) {
            ImGui::Text("Parallel detection:");
            int primary = primaryChainIdx.load();
            for (int i = 0; i < CHAIN_COUNT; i++) {
                const Chain& c = chains[i];
                if (c.messageCount > 0 && i == primary) {
                    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f),
                                       "  %4d baud: %d msg  <-- locked",
                                       c.baud, c.messageCount);
                } else if (c.messageCount > 0) {
                    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f),
                                       "  %4d baud: %d msg",
                                       c.baud, c.messageCount);
                } else {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                                       "  %4d baud: idle",
                                       c.baud);
                }
            }
        }

        // Decode-mode selector
        ImGui::LeftLabel("Decode");
        ImGui::FillWidth();
        if (ImGui::Combo(("##pager_decoder_pocsag_dm_" + name).c_str(),
                         &dmId, decodeModes.txt))
        {
            pocsag::DecodeMode dm = decodeModes.value(dmId);
            for (auto& c : chains) { c.decoder.setDecodeMode(dm); }
            if (settingsCallback) { settingsCallback(); }
        }

        // Inversion toggle
        if (ImGui::Checkbox(("Invert FSK##pager_decoder_pocsag_inv_" + name).c_str(),
                            &invert))
        {
            for (auto& c : chains) {
                c.decoder.setInverted(invert);
                c.decoder.reset();
            }
            if (settingsCallback) { settingsCallback(); }
        }

        // Audio-bandwidth low-pass filter
        if (ImGui::Checkbox(("Low Pass##pager_decoder_pocsag_lpf_" + name).c_str(),
                            &lowPass))
        {
            for (auto& c : chains) { c.dsp.setLowPass(lowPass); }
            if (settingsCallback) { settingsCallback(); }
        }

        // Symbol-diagram visualisation. In Auto mode this follows whichever
        // chain most recently produced a clean decode; in Manual mode it
        // shows the selected baud.
        ImGui::FillWidth();
        diag.draw();
    }

    void setVFO(VFOManager::VFO* vfo) override {
        this->vfo = vfo;
        vfo->setBandwidthLimits(POCSAG_VFO_BANDWIDTH, POCSAG_VFO_BANDWIDTH, true);
        vfo->setSampleRate(POCSAG_VFO_SAMPLERATE, POCSAG_VFO_BANDWIDTH);
        splitter.setInput(vfo->output);
    }

    void start() override {
        splitter.start();
        for (auto& c : chains) {
            c.dsp.start();
            c.reshape.start();
            c.dataHandler.start();
            c.diagHandler.start();
        }
    }

    void stop() override {
        for (auto& c : chains) {
            c.dataHandler.stop();
            c.diagHandler.stop();
            c.reshape.stop();
            c.dsp.stop();
        }
        splitter.stop();
    }

    // ----- Persistent settings (called from main.cpp) -----------------
    int  getBaudrate()   const { return baudrates.value(brId); }
    int  getDecodeMode() const { return (int)decodeModes.value(dmId); }
    bool getInverted()   const { return invert; }
    bool getLowPass()    const { return lowPass; }

    void setBaudrateFromConfig(int baud) {
        if (!baudrates.keyExists(baud)) { return; }
        brId = baudrates.keyId(baud);
        if (baud != 0) {
            for (int i = 0; i < CHAIN_COUNT; i++) {
                if (chains[i].baud == baud) {
                    primaryChainIdx.store(i);
                    break;
                }
            }
        }
    }

    void setDecodeModeFromConfig(int mode) {
        pocsag::DecodeMode dm = (pocsag::DecodeMode)mode;
        if (!decodeModes.keyExists(dm)) { return; }
        dmId = decodeModes.keyId(dm);
        for (auto& c : chains) { c.decoder.setDecodeMode(dm); }
    }

    void setInvertedFromConfig(bool inv) {
        invert = inv;
        for (auto& c : chains) {
            c.decoder.setInverted(inv);
            c.decoder.reset();
        }
    }

    void setLowPassFromConfig(bool lp) {
        lowPass = lp;
        for (auto& c : chains) { c.dsp.setLowPass(lp); }
    }

    // Settings-changed callback (main.cpp uses this to persist to JSON)
    void onSettingsChanged(std::function<void()> cb) {
        settingsCallback = std::move(cb);
    }

private:
    // Per-baudrate decoder chain
    struct Chain {
        int                                baud  = 0;
        int                                index = 0;
        POCSAGDecoder*                     parent = nullptr;
        dsp::stream<dsp::complex_t>        inputStream;
        POCSAGDSP                          dsp;
        pocsag::Decoder                    decoder;
        dsp::sink::Handler<uint8_t>        dataHandler;
        dsp::buffer::Reshaper<float>       reshape;
        dsp::sink::Handler<float>          diagHandler;
        int                                messageCount    = 0;
        std::time_t                        lastMessageTime = 0;
    };

    bool isAutoMode() const { return baudrates.value(brId) == 0; }

    // DSP-thread handler: feed hard decisions into this chain's decoder
    static void _chainDataHandler(uint8_t* data, int count, void* ctx) {
        Chain* c = (Chain*)ctx;
        c->decoder.process(data, count);
    }

    // DSP-thread handler: push soft decisions into the diagram, but only
    // if this chain is the currently primary one. Every chain still has
    // its reshape -> diagHandler pipeline running so the soft stream
    // drains; the callback is just a no-op for the non-primary chains.
    static void _chainDiagHandler(float* data, int count, void* ctx) {
        Chain* c = (Chain*)ctx;
        POCSAGDecoder* self = c->parent;
        if (c->index != self->primaryChainIdx.load()) { return; }

        float* buf = self->diag.acquireBuffer();
        int n = std::min(count, POCSAG_DIAG_BLOCK);
        std::memcpy(buf, data, (size_t)n * sizeof(float));
        self->diag.releaseBuffer();
    }

    // Per-chain message arrival handler. Decides whether to surface the
    // message and updates per-chain stats / primary chain selection.
    void onChainMessage(int chainIdx, const pocsag::Message& m) {
        Chain& c = chains[chainIdx];

        // Track stats only for clean messages so noise-induced BCH
        // miscorrections don't claim "successful" detection.
        if (m.errors == 0) {
            c.messageCount++;
            c.lastMessageTime = std::time(nullptr);
        }

        bool emit = false;
        if (isAutoMode()) {
            // Auto: any clean message wins. The chain that just decoded
            // becomes the primary (drives the symbol diagram).
            emit = (m.errors == 0);
            if (emit) { primaryChainIdx.store(chainIdx); }
        } else {
            // Manual: only surface messages from the selected baud.
            int selected = baudrates.value(brId);
            emit = (c.baud == selected);
        }

        if (emit) {
            flog::info("POCSAG@{}: addr={} func={} type={} corrected={} errors={} msg='{}'",
                       c.baud,
                       (uint32_t)m.address,
                       (int)m.function,
                       (int)m.type,
                       m.corrected,
                       m.errors,
                       m.content);
            if (onMessageCb) { onMessageCb(m); }
        }
    }

    std::string         name;
    VFOManager::VFO*    vfo = nullptr;

    // Parallel chain plumbing
    dsp::routing::Splitter<dsp::complex_t> splitter;
    Chain                                  chains[CHAIN_COUNT];

    // Index of the chain whose soft stream is currently displayed on the
    // diagram. Atomic because it's read by the DSP thread (diag handler)
    // and written by both the GUI thread and the message handler thread.
    std::atomic<int>                       primaryChainIdx{1};

    ImGui::SymbolDiagram                   diag;

    // UI state
    int  brId    = 0;
    int  dmId    = 0;
    bool invert  = false;
    bool lowPass = true;
    OptionList<int, int>                               baudrates;
    OptionList<pocsag::DecodeMode, pocsag::DecodeMode> decodeModes;

    MessageCallback           onMessageCb;
    std::function<void()>     settingsCallback;
};
