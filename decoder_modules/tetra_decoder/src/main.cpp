#define GImGui (ImGui::GetCurrentContext())

#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <fstream>
#include <deque>
#include <mutex>
#include <string>
#include <cstring>
#include <sys/time.h>

#include <dsp/demod/psk.h>
#include <dsp/buffer/packer.h>
#include <dsp/routing/splitter.h>
#include <dsp/stream.h>
#include <dsp/convert/mono_to_stereo.h>

#include <gui/widgets/constellation_diagram.h>
#include <gui/widgets/file_select.h>
#include <gui/widgets/volume_meter.h>

#include <utils/flog.h>
#include <utils/net.h>

#include "dsp/bit_unpacker.h"
#include "dsp/dqpsk_sym_extr.h"
#include "dsp/pi4dqpsk.h"
#include "dsp/osmotetra_dec.h"
#include "dsp/tetra_tcp_sender.h"
#include "gui_widgets.h"

extern "C" {
#include "decoder/src/tetra_events.h"
}


#define CONCAT(a, b)    ((std::string(a) + b).c_str())

#define VFO_SAMPLERATE 36000
#define VFO_BANDWIDTH 25000
#define VFO_SNAP_INTERVAL 2500.0
#define CLOCK_RECOVERY_BW 0.00628f
#define CLOCK_RECOVERY_DAMPN_F 0.707f
#define CLOCK_RECOVERY_REL_LIM 0.02f
#define RRC_TAP_COUNT 65
#define RRC_ALPHA 0.35f
#define AGC_RATE 0.02f
#define COSTAS_LOOP_BANDWIDTH 0.01f
#define FLL_LOOP_BANDWIDTH 0.006f

SDRPP_MOD_INFO {
    /* Name:            */ "tetra_demodulator",
    /* Description:     */ "TETRA demodulator: calls/SDS/status in floating window, GPS positions on TCP (type=TETRA)",
    /* Author:          */ "cropinghigh + F4JTV improvements",
    /* Version:         */ 0, 8, 5,
    /* Max instances    */ -1
};

ConfigManager config;

class TetraDemodulatorModule : public ModuleManager::Instance {
public:
    TetraDemodulatorModule(std::string name) {
        this->name = name;

        // Load config
        config.acquire();
        if (!config.conf.contains(name) || !config.conf[name].contains("mode")) {
            config.conf[name]["mode"] = decoder_mode;
            config.conf[name]["hostname"] = "localhost";
            config.conf[name]["port"] = 8355;
            config.conf[name]["sending"] = false;
        }
        if (!config.conf[name].contains("data_host")) {
            config.conf[name]["data_host"] = "127.0.0.1";
            config.conf[name]["data_port"] = 10100;
            config.conf[name]["data_enabled"] = false;
        }
        decoder_mode = config.conf[name]["mode"];
        strcpy(hostname, std::string(config.conf[name]["hostname"]).c_str());
        port = config.conf[name]["port"];
        strcpy(data_host, std::string(config.conf[name]["data_host"]).c_str());
        data_port = config.conf[name]["data_port"];
        bool startNow = config.conf[name]["sending"];
        bool startDataNow = config.conf[name]["data_enabled"];
        config.release(true);

        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, VFO_BANDWIDTH, VFO_SAMPLERATE, VFO_BANDWIDTH, VFO_BANDWIDTH, true);
        /* TETRA cells are spaced on a 12.5 kHz raster but typical European
         * deployments use a 25 kHz/2.5 kHz tuning grid for downlinks.
         * Snap to 2.5 kHz so the user can quickly land on the carrier. */
        vfo->setSnapInterval(VFO_SNAP_INTERVAL);

        // Clock recovery coefficients
        float recov_bandwidth = CLOCK_RECOVERY_BW;
        float recov_dampningFactor = CLOCK_RECOVERY_DAMPN_F;
        float recov_denominator = (1.0f + 2.0 * recov_dampningFactor * recov_bandwidth + recov_bandwidth * recov_bandwidth);
        float recov_mu = (4.0f * recov_dampningFactor * recov_bandwidth) / recov_denominator;
        float recov_omega = (4.0f * recov_bandwidth * recov_bandwidth) / recov_denominator;

        mainDemodulator.init(vfo->output, 18000, VFO_SAMPLERATE, RRC_TAP_COUNT, RRC_ALPHA, AGC_RATE, COSTAS_LOOP_BANDWIDTH, FLL_LOOP_BANDWIDTH, recov_omega, recov_mu, CLOCK_RECOVERY_REL_LIM);
        constDiagSplitter.init(&mainDemodulator.out);
        constDiagSplitter.bindStream(&constDiagStream);
        constDiagSplitter.bindStream(&demodStream);
        constDiagReshaper.init(&constDiagStream, 1024, 0);
        constDiagSink.init(&constDiagReshaper.out, _constDiagSinkHandler, this);
        symbolExtractor.init(&demodStream);
        bitsUnpacker.init(&symbolExtractor.out);

        demodSink.init(&bitsUnpacker.out, _demodSinkHandler, this);

        osmotetradecoder.init(&bitsUnpacker.out);
        resamp.init(&osmotetradecoder.out, 8000.0, audioSampleRate);
        outconv.init(&resamp.out);

        // Initialize the sink
        srChangeHandler.ctx = this;
        srChangeHandler.handler = sampleRateChangeHandler;
        stream.init(&outconv.out, &srChangeHandler, audioSampleRate);
        sigpath::sinkManager.registerStream(name, &stream);

        mainDemodulator.start();
        constDiagSplitter.start();
        constDiagReshaper.start();
        constDiagSink.start();
        symbolExtractor.start();
        bitsUnpacker.start();
        setMode();
        resamp.start();
        outconv.start();
        stream.start();
        gui::menu.registerEntry(name, menuHandler, this, this);

        if (startNow) {
            startNetwork();
        }

        // Configure (but don't necessarily start) the TCP sender to the
        // Django map collector. We always register the structured emitter
        // though — that way the in-module call/SDS/status history fills up
        // even when the TCP link is disabled. Only LIP positions go on TCP;
        // everything else lands in the floating-window history.
        tcpSender.configure(data_host, data_port);
        struct te_structured_cb cb;
        memset(&cb, 0, sizeof(cb));
        cb.ctx           = this;
        cb.on_call_setup = &TetraDemodulatorModule::onCallSetup;
        cb.on_cmce_event = &TetraDemodulatorModule::onCmceEvent;
        cb.on_sds_text   = &TetraDemodulatorModule::onSdsText;
        cb.on_status     = &TetraDemodulatorModule::onStatus;
        cb.on_lip        = &TetraDemodulatorModule::onLip;
        te_set_structured_emitter(&cb);
        tcp_enabled = startDataNow;
        if (startDataNow) {
            tcpSender.start();
        }
    }

    ~TetraDemodulatorModule() {
        if (isEnabled()) {
            disable();
        }
        te_set_structured_emitter(nullptr);
        tcpSender.stop();
        gui::menu.removeEntry(name);
        sigpath::sinkManager.unregisterStream(name);
    }

    void postInit() {}

    void enable() {
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, VFO_BANDWIDTH, VFO_SAMPLERATE, VFO_BANDWIDTH, VFO_BANDWIDTH, true);
        vfo->setSnapInterval(VFO_SNAP_INTERVAL);
        mainDemodulator.setInput(vfo->output);
        mainDemodulator.start();
        constDiagSplitter.start();
        constDiagReshaper.start();
        constDiagSink.start();
        symbolExtractor.start();
        bitsUnpacker.start();
        setMode();
        resamp.start();
        outconv.start();
        stream.start();
        enabled = true;
    }

    void disable() {
        mainDemodulator.stop();
        constDiagSplitter.stop();
        constDiagReshaper.stop();
        constDiagSink.stop();
        symbolExtractor.stop();
        bitsUnpacker.stop();
        osmotetradecoder.stop();
        demodSink.stop();
        resamp.stop();
        outconv.stop();
        stream.stop();
        sigpath::vfoManager.deleteVFO(vfo);
        enabled = false;
    }

    bool isEnabled() { return enabled; }

private:
    void startNetwork() {
        stopNetwork();
        try {
            conn = net::openudp(hostname, port);
        } catch (std::runtime_error& e) {
            flog::error("Network error: %s\n", e.what());
        }
    }

    void stopNetwork() {
        if (conn) { conn->close(); }
    }

    void setMode() {
        if (decoder_mode == 0) {
            demodSink.stop();
            osmotetradecoder.start();
        } else {
            osmotetradecoder.stop();
            demodSink.start();
        }
        config.acquire();
        config.conf[name]["mode"] = decoder_mode;
        config.release(true);
    }

    // ----------------------------------------------------------------------
    //  GUI helpers (kept inside the class so they can stay private)
    // ----------------------------------------------------------------------
    static void drawDemodulatorSection(TetraDemodulatorModule* _this, float menuWidth) {
        // Constellation
        ImGui::TextUnformatted("Constellation");
        ImGui::SetNextItemWidth(menuWidth);
        _this->constDiag.draw();

        // Sync LED
        ImGui::StatusLed(_this->symbolExtractor.sync);
        ImGui::SameLine();
        ImGui::Text("Symbol sync: %s",
                    _this->symbolExtractor.sync ? "locked" : "lost");

        // Signal quality bar
        float avg = 1.0f - _this->symbolExtractor.standarderr;
        ImGui::TextUnformatted("Signal quality");
        ImGui::SetNextItemWidth(menuWidth);
        ImGui::SigQualityMeter(avg, 0.5f, 1.0f);
    }

    static void drawModeSelector(TetraDemodulatorModule* _this, float menuWidth) {
        ImGui::TextUnformatted("Output mode");
        ImGui::Columns(2, CONCAT("TetraModeColumns##_", _this->name), false);
        if (ImGui::RadioButton(CONCAT("OSMO-TETRA##_", _this->name), _this->decoder_mode == 0)
            && _this->decoder_mode != 0) {
            _this->decoder_mode = 0;
            _this->setMode();
        }
        ImGui::NextColumn();
        if (ImGui::RadioButton(CONCAT("NETSYMS##_", _this->name), _this->decoder_mode == 1)
            && _this->decoder_mode != 1) {
            _this->decoder_mode = 1;
            _this->setMode();
        }
        ImGui::Columns(1);
    }

    static void drawOsmoDecoderUI(TetraDemodulatorModule* _this, float menuWidth) {
        int dec_st = _this->osmotetradecoder.getRxState();

        // ---- Decoder status header -----------------------------------
        ImU32 statusColor =
              (dec_st == 0) ? IM_COL32(230, 5, 5, 255)
            : (dec_st == 2) ? IM_COL32(5, 230, 5, 255)
                            : IM_COL32(230, 230, 5, 255);
        ImGui::StatusLed(dec_st == 2,
                         0.0f,
                         statusColor,
                         statusColor);
        ImGui::SameLine();
        ImGui::Text("Decoder: %s",
            (dec_st == 0) ? "Unlocked"
          : (dec_st == 2) ? "Locked"
                          : "Know next start");

        if (dec_st != 2) { style::beginDisabled(); }

        // ---- Frame counters ------------------------------------------
        ImGui::Spacing();
        ImGui::FrameCounterDisplay(
            _this->osmotetradecoder.getCurrHyperframe(),
            _this->osmotetradecoder.getCurrMultiframe(),
            _this->osmotetradecoder.getCurrFrame(),
            menuWidth);

        // ---- Timeslot grid -------------------------------------------
        ImGui::Spacing();
        ImGui::TextUnformatted("Timeslots");
        int slotContents[4] = {
            _this->osmotetradecoder.getTimeslotContent(0),
            _this->osmotetradecoder.getTimeslotContent(1),
            _this->osmotetradecoder.getTimeslotContent(2),
            _this->osmotetradecoder.getTimeslotContent(3)
        };
        int slotEnc[4] = {
            _this->osmotetradecoder.getEncryptionModeSlot(0),
            _this->osmotetradecoder.getEncryptionModeSlot(1),
            _this->osmotetradecoder.getEncryptionModeSlot(2),
            _this->osmotetradecoder.getEncryptionModeSlot(3)
        };
        ImGui::TimeslotIndicator(slotContents, slotEnc, menuWidth);

        // ---- Per-slot SSI + call state ------------------------------
        // Sessions 2+ refine these from CMCE D-SETUP/D-CONNECT; right
        // now we just show whichever SSI was last observed in a MAC
        // RESOURCE PDU on each slot, with a freshness indicator.
        ImGui::Spacing();
        ImGui::TextUnformatted("Slot identities (SSI)");
        int slotSsi[4] = {
            _this->osmotetradecoder.getSsiSlot(0),
            _this->osmotetradecoder.getSsiSlot(1),
            _this->osmotetradecoder.getSsiSlot(2),
            _this->osmotetradecoder.getSsiSlot(3)
        };
        int slotCS[4] = {
            _this->osmotetradecoder.getCallStateSlot(0),
            _this->osmotetradecoder.getCallStateSlot(1),
            _this->osmotetradecoder.getCallStateSlot(2),
            _this->osmotetradecoder.getCallStateSlot(3)
        };
        uint64_t slotAge[4] = {
            _this->osmotetradecoder.getSsiSlotAgeMs(0),
            _this->osmotetradecoder.getSsiSlotAgeMs(1),
            _this->osmotetradecoder.getSsiSlotAgeMs(2),
            _this->osmotetradecoder.getSsiSlotAgeMs(3)
        };
        ImGui::SlotInfoRow(slotSsi, slotCS, slotAge, menuWidth);

        // ---- CRC status ---------------------------------------------
        int crc_failed = _this->osmotetradecoder.getLastCrcFail();
        ImGui::Spacing();
        ImGui::StatusLed(!crc_failed);
        ImGui::SameLine();
        ImGui::TextUnformatted("Last CRC: ");
        ImGui::SameLine();
        if (crc_failed) {
            ImGui::TextColored(ImVec4(0.95f, 0.25f, 0.25f, 1.0f), "FAIL");
            style::beginDisabled();
        } else {
            ImGui::TextColored(ImVec4(0.25f, 0.95f, 0.25f, 1.0f), "PASS");
        }

        // ---- Cell info collapsing header -----------------------------
        ImGui::Spacing();
        if (ImGui::CollapsingHeader(CONCAT("Cell info##_", _this->name),
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            char buf[16];
            ImGui::Columns(3, CONCAT("CellId##_", _this->name), false);
            std::snprintf(buf, 16, "%03d", _this->osmotetradecoder.getMcc());
            ImGui::LabeledValue("MCC", buf);
            ImGui::NextColumn();
            std::snprintf(buf, 16, "%03d", _this->osmotetradecoder.getMnc());
            ImGui::LabeledValue("MNC", buf);
            ImGui::NextColumn();
            std::snprintf(buf, 16, "0x%02x", _this->osmotetradecoder.getCc());
            ImGui::LabeledValue("CC", buf);
            ImGui::Columns(1);

            ImGui::Spacing();
            int dl_usg = _this->osmotetradecoder.getDlUsage();
            int ul_usg = _this->osmotetradecoder.getUlUsage();
            const char* dl_usage_str =
                  (dl_usg == 0) ? "Unalloc"
                : (dl_usg == 1) ? "Assigned ctl"
                : (dl_usg == 2) ? "Common ctl"
                : (dl_usg == 3) ? "Reserved"
                                : "Traffic";
            const char* ul_usage_str = (ul_usg == 0) ? "Unalloc" : "Traffic";
            ImGui::FreqRow("DL", _this->osmotetradecoder.getDlFreq(), dl_usage_str);
            ImGui::FreqRow("UL", _this->osmotetradecoder.getUlFreq(), ul_usage_str);

            ImGui::Spacing();
            char access1Str[16], access2Str[16];
            std::snprintf(access1Str, 16, "%c / %d",
                _this->osmotetradecoder.getAccess1Code(),
                _this->osmotetradecoder.getAccess1());
            std::snprintf(access2Str, 16, "%c / %d",
                _this->osmotetradecoder.getAccess2Code(),
                _this->osmotetradecoder.getAccess2());
            ImGui::Columns(2, CONCAT("AccessCols##_", _this->name), false);
            ImGui::LabeledValue("Access 1", access1Str);
            ImGui::NextColumn();
            ImGui::LabeledValue("Access 2", access2Str);
            ImGui::Columns(1);
        }

        // ---- Services & flags ----------------------------------------
        ImGui::Spacing();
        if (ImGui::CollapsingHeader(CONCAT("Services & capabilities##_", _this->name),
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            // Two-column grid of pills. Each pill is short -> two columns fit easily.
            ImGui::Columns(2, CONCAT("SrvCols##_", _this->name), false);

            ImGui::StatusPill("Adv. link",  _this->osmotetradecoder.getAdvancedLink());
            ImGui::SameLine();
            ImGui::StatusPill("Encryption", _this->osmotetradecoder.getAirEncryption(),
                              IM_COL32(170, 110, 30, 240));
            ImGui::NextColumn();

            ImGui::StatusPill("Voice",      _this->osmotetradecoder.getVoiceService());
            ImGui::SameLine();
            ImGui::StatusPill("Circuit data", _this->osmotetradecoder.getCircuitData());
            ImGui::NextColumn();

            ImGui::StatusPill("SNDCP",      _this->osmotetradecoder.getSndcpData());
            ImGui::SameLine();
            ImGui::StatusPill("Normal mode", _this->osmotetradecoder.getNormalMode());
            ImGui::NextColumn();

            ImGui::StatusPill("Migration",   _this->osmotetradecoder.getMigrationSupported());
            ImGui::SameLine();
            ImGui::StatusPill("Never min.",  _this->osmotetradecoder.getNeverMinimumMode());
            ImGui::NextColumn();

            ImGui::StatusPill("Priority cell", _this->osmotetradecoder.getPriorityCell());
            ImGui::NextColumn();

            ImGui::StatusPill("Dereg req.",  _this->osmotetradecoder.getDeregMandatory(),
                              IM_COL32(170, 110, 30, 240));
            ImGui::SameLine();
            ImGui::StatusPill("Reg req.",    _this->osmotetradecoder.getRegMandatory(),
                              IM_COL32(170, 110, 30, 240));
            ImGui::Columns(1);
        }

        if (crc_failed) { style::endDisabled(); }
        if (dec_st != 2) { style::endDisabled(); }
    }

    static void drawNetsymsUI(TetraDemodulatorModule* _this, float menuWidth) {
        ImGui::StatusLed(_this->tsfound);
        ImGui::SameLine();
        ImGui::TextUnformatted("Training sequences");

        bool netActive = (_this->conn && _this->conn->isOpen());

        if (netActive) { style::beginDisabled(); }
        ImGui::TextUnformatted("Destination");
        ImGui::SetNextItemWidth(menuWidth * 0.65f);
        if (ImGui::InputText(CONCAT("##_tetrademod_host_", _this->name),
                             _this->hostname, 1023)) {
            config.acquire();
            config.conf[_this->name]["hostname"] = _this->hostname;
            config.release(true);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(CONCAT("##_tetrademod_port_", _this->name),
                            &(_this->port), 0, 0)) {
            config.acquire();
            config.conf[_this->name]["port"] = _this->port;
            config.release(true);
        }
        if (netActive) { style::endDisabled(); }

        if (netActive &&
            ImGui::Button(CONCAT("Stop UDP##_tetrademod_net_", _this->name),
                          ImVec2(menuWidth, 0))) {
            _this->stopNetwork();
            config.acquire();
            config.conf[_this->name]["sending"] = false;
            config.release(true);
        } else if (!netActive &&
                   ImGui::Button(CONCAT("Start UDP##_tetrademod_net_", _this->name),
                                 ImVec2(menuWidth, 0))) {
            _this->startNetwork();
            config.acquire();
            config.conf[_this->name]["sending"] = true;
            config.release(true);
        }

        ImGui::StatusLed(netActive);
        ImGui::SameLine();
        ImGui::TextUnformatted(netActive ? "UDP active" : "UDP idle");
    }

    // ----------------------------------------------------------------------
    //  Main menu handler
    // ----------------------------------------------------------------------
    static void menuHandler(void* ctx) {
        TetraDemodulatorModule* _this = (TetraDemodulatorModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) { style::beginDisabled(); }

        // --- Section: Demodulator ----------------------------------------
        if (ImGui::CollapsingHeader(CONCAT("Demodulator##_", _this->name),
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            drawDemodulatorSection(_this, menuWidth);
        }

        // --- Section: Mode -----------------------------------------------
        ImGui::Spacing();
        drawModeSelector(_this, menuWidth);
        ImGui::Spacing();

        // --- Section: Decoder (OSMO) or Network (NETSYMS) ----------------
        if (_this->decoder_mode == 0) {
            if (ImGui::CollapsingHeader(CONCAT("Decoder##_", _this->name),
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                drawOsmoDecoderUI(_this, menuWidth);
            }
        } else {
            if (ImGui::CollapsingHeader(CONCAT("Network output##_", _this->name),
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                drawNetsymsUI(_this, menuWidth);
            }
        }

        // --- Section: Data export (TCP feed, AIS/ADSB-style UI) ----------
        // No CollapsingHeader here — the AIS / ADS-B / APRS modules render
        // their TCP block flat inside the menu, just a separator + title.
        drawDataExportUI(_this, menuWidth);

        if (!_this->enabled) { style::endDisabled(); }

        // Floating window with call / SDS / status history. It does NOT
        // belong inside menuHandler's enable/disable bracket — we want
        // users to still browse the history even when the module is off. */
        if (_this->showDataWindow) {
            drawDataWindow(_this);
        }
    }

    // ----------------------------------------------------------------------
    //  TCP map output — visually identical to the AIS / ADS-B / APRS modules.
    //
    //  Layout (top-to-bottom):
    //
    //      ─────────────────────────────────────
    //      TCP map output
    //      Host  [______________________________]
    //      Port  [______________________________]
    //      [X] Enable TCP    connected
    //
    //  Plus, at the bottom, a button to open the "TETRA Data" floating window
    //  (calls / SDS / status). That extra button is the only divergence from
    //  AIS/ADSB — they don't have a side window, we do.
    // ----------------------------------------------------------------------
    static void drawDataExportUI(TetraDemodulatorModule* _this, float menuWidth) {
        ImGui::Separator();
        ImGui::TextUnformatted("TCP map output");

        // Host
        ImGui::LeftLabel("Host");
        ImGui::FillWidth();
        bool tcpChanged = ImGui::InputText(
            CONCAT("##tetra_tcp_host_", _this->name),
            _this->data_host, sizeof(_this->data_host) - 1);

        // Port
        ImGui::LeftLabel("Port");
        ImGui::FillWidth();
        if (ImGui::InputInt(CONCAT("##tetra_tcp_port_", _this->name),
                            &_this->data_port, 0, 0)) {
            if (_this->data_port < 0) _this->data_port = 0;
            if (_this->data_port > 65535) _this->data_port = 65535;
            tcpChanged = true;
        }
        if (tcpChanged) {
            _this->tcpSender.configure(_this->data_host, _this->data_port);
            config.acquire();
            config.conf[_this->name]["data_host"] = std::string(_this->data_host);
            config.conf[_this->name]["data_port"] = _this->data_port;
            config.release(true);
        }

        // Enable checkbox + inline status word
        if (ImGui::Checkbox(CONCAT("Enable TCP##tetra_tcp_en_", _this->name),
                            &_this->tcp_enabled)) {
            if (_this->tcp_enabled) {
                _this->tcpSender.configure(_this->data_host, _this->data_port);
                _this->tcpSender.start();
            } else {
                _this->tcpSender.stop();
            }
            config.acquire();
            config.conf[_this->name]["data_enabled"] = _this->tcp_enabled;
            config.release(true);
        }
        ImGui::SameLine();
        if (!_this->tcp_enabled) {
            ImGui::TextDisabled("disabled");
        } else if (_this->tcpSender.isConnected()) {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "connected");
        } else {
            ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "enabled");
        }

        // Side button to open the floating window with calls / SDS / status.
        ImGui::Spacing();
        if (ImGui::Button(CONCAT("Show TETRA Data window##_", _this->name),
                          ImVec2(menuWidth, 0))) {
            _this->showDataWindow = true;
        }
    }

    // ----------------------------------------------------------------------
    //  Structured event callbacks (registered with te_set_structured_emitter)
    //
    //  These run on the decoder thread. They are deliberately tiny: they
    //  format a small struct, push it onto the history deque under a mutex,
    //  trim if needed, and return. UI rendering happens later, on the main
    //  thread, by walking the deque under the same mutex.
    //
    //  The LIP callback also synthesises the unified TCP JSON line (same
    //  schema as AIS / ADS-B / APRS / DSD-FME / radiosonde) and queues it
    //  for the TCP sender — but does NOT add the LIP event to the history,
    //  since positions belong on the map, not in the call/SDS/status log.
    // ----------------------------------------------------------------------

    /* UTC-now ISO8601 helper (shared by all callbacks, plus the LIP JSON
     * formatter). Output is "YYYY-MM-DDTHH:MM:SS.mmmZ". Each field is
     * explicitly clamped so the compiler can prove the format fits and
     * -Wformat-truncation stays quiet. */
    static std::string nowUtcIso8601() {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        struct tm tm_;
        gmtime_r(&tv.tv_sec, &tm_);
        int ms = (int)(tv.tv_usec / 1000);
        if (ms < 0) ms = 0;
        if (ms > 999) ms = 999;
        int yr  = tm_.tm_year + 1900; if (yr < 0)  yr = 0;  if (yr > 9999) yr = 9999;
        int mo  = tm_.tm_mon  + 1;    if (mo < 1)  mo = 1;  if (mo > 12)   mo = 12;
        int day = tm_.tm_mday;        if (day < 1) day = 1; if (day > 31)  day = 31;
        int hr  = tm_.tm_hour;        if (hr < 0)  hr = 0;  if (hr > 23)   hr = 23;
        int mn  = tm_.tm_min;         if (mn < 0)  mn = 0;  if (mn > 59)   mn = 59;
        int sc  = tm_.tm_sec;         if (sc < 0)  sc = 0;  if (sc > 60)   sc = 60;
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                      yr, mo, day, hr, mn, sc, ms);
        return std::string(buf);
    }

    /* Split nowUtcIso8601 into separate date / time strings, the way the
     * AIS / ADS-B / APRS modules format their TCP lines. */
    static void nowUtcDateTime(std::string& date_out, std::string& time_out) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        struct tm tm_;
        gmtime_r(&tv.tv_sec, &tm_);
        int yr  = tm_.tm_year + 1900; if (yr < 0)  yr = 0;  if (yr > 9999) yr = 9999;
        int mo  = tm_.tm_mon  + 1;    if (mo < 1)  mo = 1;  if (mo > 12)   mo = 12;
        int day = tm_.tm_mday;        if (day < 1) day = 1; if (day > 31)  day = 31;
        int hr  = tm_.tm_hour;        if (hr < 0)  hr = 0;  if (hr > 23)   hr = 23;
        int mn  = tm_.tm_min;         if (mn < 0)  mn = 0;  if (mn > 59)   mn = 59;
        int sc  = tm_.tm_sec;         if (sc < 0)  sc = 0;  if (sc > 60)   sc = 60;
        char d[16], t[16];
        std::snprintf(d, sizeof(d), "%04d-%02d-%02d", yr, mo, day);
        std::snprintf(t, sizeof(t), "%02d:%02d:%02d", hr, mn, sc);
        date_out = d;
        time_out = t;
    }

    /* Trim a history deque to MAX_HISTORY entries (called under the mutex). */
    template <typename T>
    static void trimHistory(std::deque<T>& q) {
        while (q.size() > MAX_HISTORY) q.pop_front();
    }

    static void onCallSetup(int caller, int callee, const char *call_type,
                            int slot, int call_id, void *ctx) {
        TetraDemodulatorModule* _this = (TetraDemodulatorModule*)ctx;
        CallEvent ev;
        ev.ts = nowUtcIso8601();
        ev.kind = "setup";
        ev.caller = caller;
        ev.callee = callee;
        ev.call_id = call_id;
        ev.slot = slot;
        ev.call_type = call_type ? call_type : "unknown";
        std::lock_guard<std::mutex> lk(_this->historyMtx);
        _this->callHistory.push_back(std::move(ev));
        trimHistory(_this->callHistory);
    }

    static void onCmceEvent(const char *event_name, int call_id, int cause,
                            int slot, void *ctx) {
        TetraDemodulatorModule* _this = (TetraDemodulatorModule*)ctx;
        CallEvent ev;
        ev.ts = nowUtcIso8601();
        /* Normalise the event_name into a short tag. The strings used by
         * the decoder ("call_connect", "call_release", "call_proceeding",
         * "tx_ceased") are kept verbatim in the history; the UI can group
         * them as needed. */
        ev.kind = event_name ? event_name : "cmce";
        ev.call_id = call_id;
        ev.cause = cause;
        ev.slot = slot;
        std::lock_guard<std::mutex> lk(_this->historyMtx);
        _this->callHistory.push_back(std::move(ev));
        trimHistory(_this->callHistory);
    }

    static void onSdsText(int src, int dst, int proto, const char *text,
                          void *ctx) {
        TetraDemodulatorModule* _this = (TetraDemodulatorModule*)ctx;
        SdsEvent ev;
        ev.ts = nowUtcIso8601();
        ev.src = src;
        ev.dst = dst;
        ev.proto = proto;
        ev.text = text ? text : "";
        std::lock_guard<std::mutex> lk(_this->historyMtx);
        _this->sdsHistory.push_back(std::move(ev));
        trimHistory(_this->sdsHistory);
    }

    static void onStatus(int src, int dst, int code, void *ctx) {
        TetraDemodulatorModule* _this = (TetraDemodulatorModule*)ctx;
        StatusEvent ev;
        ev.ts = nowUtcIso8601();
        ev.src = src;
        ev.dst = dst;
        ev.code = code;
        std::lock_guard<std::mutex> lk(_this->historyMtx);
        _this->statusHistory.push_back(std::move(ev));
        trimHistory(_this->statusHistory);
    }

    /* LIP — format as a unified JSON line and queue it on the TCP sender.
     * The line is intentionally NOT added to any in-memory history: this is
     * the *only* event type that goes on the wire to the Django map. */
    static void onLip(int src, double lat, double lon,
                      int acc, int vel, int dir, void *ctx) {
        TetraDemodulatorModule* _this = (TetraDemodulatorModule*)ctx;

        std::string date, time;
        nowUtcDateTime(date, time);

        /* Same fields and naming as the AIS / ADS-B / APRS / DSD-FME /
         * radiosonde modules: name, mmsi, date, time, lat, lon, type,
         * speed, info. The "mmsi" field is reused for SSI on purpose so
         * the existing Django listener (listen_sdr / listen_ais) can key
         * objects without schema changes — every contact has a stable
         * numeric identity. */
        /* Build the "info" string. Use "unk" instead of negative numbers
         * for unknown fields, mirroring how AIS / ADS-B do it. */
        char info[80];
        char accstr[16], dirstr[16];
        if (acc >= 0) std::snprintf(accstr, sizeof(accstr), "%dm", acc);
        else          std::snprintf(accstr, sizeof(accstr), "unk");
        if (dir >= 0) std::snprintf(dirstr, sizeof(dirstr), "%ddeg", dir);
        else          std::snprintf(dirstr, sizeof(dirstr), "unk");
        std::snprintf(info, sizeof(info), "acc=%s dir=%s", accstr, dirstr);

        char line[512];
        if (vel >= 0) {
            std::snprintf(line, sizeof(line),
                "{\"name\":\"SSI:%d\",\"mmsi\":%d,"
                "\"date\":\"%s\",\"time\":\"%s\","
                "\"lat\":%.7f,\"lon\":%.7f,"
                "\"type\":\"TETRA\",\"speed\":%d,"
                "\"info\":\"%s\"}",
                src, src, date.c_str(), time.c_str(),
                lat, lon, vel, info);
        } else {
            std::snprintf(line, sizeof(line),
                "{\"name\":\"SSI:%d\",\"mmsi\":%d,"
                "\"date\":\"%s\",\"time\":\"%s\","
                "\"lat\":%.7f,\"lon\":%.7f,"
                "\"type\":\"TETRA\",\"speed\":null,"
                "\"info\":\"%s\"}",
                src, src, date.c_str(), time.c_str(),
                lat, lon, info);
        }
        _this->tcpSender.send(std::string(line));
    }

    // ----------------------------------------------------------------------
    //  Floating "TETRA Data" window — calls / SDS / status (no positions).
    //
    //  Mirrors the layout convention used by the DSD-FME and POCSAG modules
    //  in our toolbox: opened via a button in the side menu, ImGui::Begin
    //  with a close cross, mainWindow.lockWaterfallControls latched while
    //  hovered so scroll-wheel inside the window doesn't retune the VFO.
    // ----------------------------------------------------------------------
    static void drawDataWindow(TetraDemodulatorModule* _this) {
        ImGui::SetNextWindowSize(ImVec2(720, 520), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(CONCAT("TETRA Data##_window_", _this->name),
                          &_this->showDataWindow,
                          ImGuiWindowFlags_NoCollapse)) {
            ImGui::End();
            return;
        }

        /* Suppress waterfall scroll capture while the user interacts with
         * this window — same trick as in the AIS / ADS-B / APRS / DSD-FME
         * modules to keep the VFO from drifting under the wheel. */
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup |
                                   ImGuiHoveredFlags_AllowWhenBlockedByActiveItem |
                                   ImGuiHoveredFlags_ChildWindows)) {
            gui::mainWindow.lockWaterfallControls = true;
        }

        if (ImGui::BeginTabBar("##tetra_data_tabs")) {

            /* ---------- Tab: Calls ------------------------------------- */
            if (ImGui::BeginTabItem("Calls")) {
                std::lock_guard<std::mutex> lk(_this->historyMtx);

                if (ImGui::Button("Clear##calls", ImVec2(80, 0))) {
                    _this->callHistory.clear();
                }
                ImGui::SameLine();
                ImGui::Text("%zu event(s)", _this->callHistory.size());

                if (ImGui::BeginTable("##calls_table", 6,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Time",    ImGuiTableColumnFlags_WidthFixed, 90.0f);
                    ImGui::TableSetupColumn("Kind",    ImGuiTableColumnFlags_WidthFixed, 100.0f);
                    ImGui::TableSetupColumn("Caller",  ImGuiTableColumnFlags_WidthFixed, 90.0f);
                    ImGui::TableSetupColumn("Callee",  ImGuiTableColumnFlags_WidthFixed, 90.0f);
                    ImGui::TableSetupColumn("Type",    ImGuiTableColumnFlags_WidthFixed, 90.0f);
                    ImGui::TableSetupColumn("ID/cause/slot");
                    ImGui::TableHeadersRow();

                    /* Walk newest-first for readability. */
                    for (auto it = _this->callHistory.rbegin();
                              it != _this->callHistory.rend(); ++it) {
                        const auto& ev = *it;
                        ImGui::TableNextRow();
                        /* Time: just HH:MM:SS from the trailing ISO string. */
                        ImGui::TableSetColumnIndex(0);
                        if (ev.ts.size() >= 19) {
                            ImGui::TextUnformatted(ev.ts.c_str() + 11, ev.ts.c_str() + 19);
                        } else {
                            ImGui::TextUnformatted(ev.ts.c_str());
                        }
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(ev.kind.c_str());
                        ImGui::TableSetColumnIndex(2);
                        if (ev.caller) ImGui::Text("%d", ev.caller);
                        else           ImGui::TextDisabled("—");
                        ImGui::TableSetColumnIndex(3);
                        if (ev.callee) ImGui::Text("%d", ev.callee);
                        else           ImGui::TextDisabled("—");
                        ImGui::TableSetColumnIndex(4);
                        if (!ev.call_type.empty())
                            ImGui::TextUnformatted(ev.call_type.c_str());
                        else
                            ImGui::TextDisabled("—");
                        ImGui::TableSetColumnIndex(5);
                        ImGui::Text("id=%d c=%d s=%d", ev.call_id, ev.cause, ev.slot);
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }

            /* ---------- Tab: SDS Messages ----------------------------- */
            if (ImGui::BeginTabItem("SDS Messages")) {
                std::lock_guard<std::mutex> lk(_this->historyMtx);

                if (ImGui::Button("Clear##sds", ImVec2(80, 0))) {
                    _this->sdsHistory.clear();
                }
                ImGui::SameLine();
                ImGui::Text("%zu message(s)", _this->sdsHistory.size());

                if (ImGui::BeginTable("##sds_table", 5,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Time",  ImGuiTableColumnFlags_WidthFixed, 90.0f);
                    ImGui::TableSetupColumn("From",  ImGuiTableColumnFlags_WidthFixed, 90.0f);
                    ImGui::TableSetupColumn("To",    ImGuiTableColumnFlags_WidthFixed, 90.0f);
                    ImGui::TableSetupColumn("Proto", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                    ImGui::TableSetupColumn("Text");
                    ImGui::TableHeadersRow();

                    for (auto it = _this->sdsHistory.rbegin();
                              it != _this->sdsHistory.rend(); ++it) {
                        const auto& ev = *it;
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        if (ev.ts.size() >= 19)
                            ImGui::TextUnformatted(ev.ts.c_str() + 11, ev.ts.c_str() + 19);
                        else
                            ImGui::TextUnformatted(ev.ts.c_str());
                        ImGui::TableSetColumnIndex(1);
                        if (ev.src) ImGui::Text("%d", ev.src); else ImGui::TextDisabled("—");
                        ImGui::TableSetColumnIndex(2);
                        if (ev.dst) ImGui::Text("%d", ev.dst); else ImGui::TextDisabled("—");
                        ImGui::TableSetColumnIndex(3);
                        ImGui::Text("0x%02X", ev.proto);
                        ImGui::TableSetColumnIndex(4);
                        ImGui::TextWrapped("%s", ev.text.c_str());
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }

            /* ---------- Tab: Status codes ---------------------------- */
            if (ImGui::BeginTabItem("Status")) {
                std::lock_guard<std::mutex> lk(_this->historyMtx);

                if (ImGui::Button("Clear##status", ImVec2(80, 0))) {
                    _this->statusHistory.clear();
                }
                ImGui::SameLine();
                ImGui::Text("%zu code(s)", _this->statusHistory.size());

                if (ImGui::BeginTable("##status_table", 4,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                    ImGui::TableSetupColumn("From", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                    ImGui::TableSetupColumn("To",   ImGuiTableColumnFlags_WidthFixed, 100.0f);
                    ImGui::TableSetupColumn("Code");
                    ImGui::TableHeadersRow();

                    for (auto it = _this->statusHistory.rbegin();
                              it != _this->statusHistory.rend(); ++it) {
                        const auto& ev = *it;
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        if (ev.ts.size() >= 19)
                            ImGui::TextUnformatted(ev.ts.c_str() + 11, ev.ts.c_str() + 19);
                        else
                            ImGui::TextUnformatted(ev.ts.c_str());
                        ImGui::TableSetColumnIndex(1);
                        if (ev.src) ImGui::Text("%d", ev.src); else ImGui::TextDisabled("—");
                        ImGui::TableSetColumnIndex(2);
                        if (ev.dst == 0xFFFFFF) ImGui::TextUnformatted("ALL");
                        else if (ev.dst)        ImGui::Text("%d", ev.dst);
                        else                    ImGui::TextDisabled("—");
                        ImGui::TableSetColumnIndex(3);
                        /* Show both hex (canonical for TETRA codes) and
                         * decimal (handy for operator code books that
                         * list "10-codes" as plain numbers). */
                        if (ev.code >= 0x8000)
                            ImGui::Text("0x%04X (%d) ETSI", ev.code, ev.code);
                        else
                            ImGui::Text("0x%04X (%d) operator", ev.code, ev.code);
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::End();
    }

    static void _constDiagSinkHandler(dsp::complex_t* data, int count, void* ctx) {
        TetraDemodulatorModule* _this = (TetraDemodulatorModule*)ctx;
        dsp::complex_t* cdBuff = _this->constDiag.acquireBuffer();
        if (count == 1024) {
            memcpy(cdBuff, data, count * sizeof(dsp::complex_t));
        }
        _this->constDiag.releaseBuffer();
    }

    static void _demodSinkHandler(uint8_t* data, int count, void* ctx) {
        TetraDemodulatorModule* _this = (TetraDemodulatorModule*)ctx;
        if (_this->conn && _this->conn->isOpen()) {
            _this->conn->send(data, count);
        }
        for (int j = 0; j < count; j++) {
            for (int i = 0; i < 44; i++) {
                _this->tsfind_buffer[i] = _this->tsfind_buffer[i + 1];
            }
            _this->tsfind_buffer[44] = data[j];
            if (!memcmp(_this->tsfind_buffer, training_seq_n, sizeof(training_seq_n)) ||
                !memcmp(_this->tsfind_buffer, training_seq_p, sizeof(training_seq_p)) ||
                !memcmp(_this->tsfind_buffer, training_seq_q, sizeof(training_seq_q)) ||
                !memcmp(_this->tsfind_buffer, training_seq_N, sizeof(training_seq_N)) ||
                !memcmp(_this->tsfind_buffer, training_seq_P, sizeof(training_seq_P)) ||
                !memcmp(_this->tsfind_buffer, training_seq_x, sizeof(training_seq_x)) ||
                !memcmp(_this->tsfind_buffer, training_seq_X, sizeof(training_seq_X)) ||
                !memcmp(_this->tsfind_buffer, training_seq_y, sizeof(training_seq_y))) {
                _this->tsfound = true;
                _this->symsbeforeexpire = 2048;
            }
            if (_this->symsbeforeexpire > 0) {
                _this->symsbeforeexpire--;
                if (_this->symsbeforeexpire == 0) {
                    _this->tsfound = false;
                }
            }
        }
    }

    static void sampleRateChangeHandler(float sampleRate, void* ctx) {
        TetraDemodulatorModule* _this = (TetraDemodulatorModule*)ctx;
        _this->audioSampleRate = sampleRate;
        _this->resamp.stop();
        _this->resamp.setOutSamplerate(_this->audioSampleRate);
        _this->resamp.start();
    }

    std::string name;
    bool enabled = true;

    VFOManager::VFO* vfo;

    dsp::demod::PI4DQPSK mainDemodulator;
    dsp::routing::Splitter<dsp::complex_t> constDiagSplitter;
    dsp::stream<dsp::complex_t> constDiagStream;
    dsp::buffer::Reshaper<dsp::complex_t> constDiagReshaper;
    dsp::sink::Handler<dsp::complex_t> constDiagSink;
    ImGui::ConstellationDiagram constDiag;

    dsp::stream<dsp::complex_t> demodStream;

    dsp::DQPSKSymbolExtractor symbolExtractor;
    dsp::BitUnpacker bitsUnpacker;

    dsp::sink::Handler<uint8_t> demodSink;

    dsp::osmotetradec osmotetradecoder;

    EventHandler<float> srChangeHandler;
    dsp::multirate::RationalResampler<float> resamp;
    dsp::convert::MonoToStereo outconv;
    SinkManager::Stream stream;
    double audioSampleRate = 48000.0;

    int decoder_mode = 0;

    // Sequences from osmo-tetra-sq5bpf source
    /* 9.4.4.3.2 Normal Training Sequence */
    static const constexpr uint8_t training_seq_n[22] = { 1,1, 0,1, 0,0, 0,0, 1,1, 1,0, 1,0, 0,1, 1,1, 0,1, 0,0 };
    static const constexpr uint8_t training_seq_p[22] = { 0,1, 1,1, 1,0, 1,0, 0,1, 0,0, 0,0, 1,1, 0,1, 1,1, 1,0 };
    static const constexpr uint8_t training_seq_q[22] = { 1,0, 1,1, 0,1, 1,1, 0,0, 0,0, 0,1, 1,0, 1,0, 1,1, 0,1 };
    static const constexpr uint8_t training_seq_N[33] = { 1,1,1, 0,0,1, 1,0,1, 1,1,1, 0,0,0, 1,1,1, 1,0,0, 0,1,1, 1,1,0, 0,0,0, 0,0,0 };
    static const constexpr uint8_t training_seq_P[33] = { 1,0,1, 0,1,1, 1,1,1, 1,0,1, 0,1,0, 1,0,1, 1,1,0, 0,0,1, 1,0,0, 0,1,0, 0,1,0 };

    /* 9.4.4.3.3 Extended training sequence */
    static const constexpr uint8_t training_seq_x[30] = { 1,0, 0,1, 1,1, 0,1, 0,0, 0,0, 1,1, 1,0, 1,0, 0,1, 1,1, 0,1, 0,0, 0,0, 1,1 };
    static const constexpr uint8_t training_seq_X[45] = { 0,1,1,1,0,0,1,1,0,1,0,0,0,0,1,0,0,0,1,1,1,0,1,1,0,1,0,1,0,1,1,1,1,1,0,1,0,0,0,0,0,1,1,1,0 };

    /* 9.4.4.3.4 Synchronization training sequence */
    static const constexpr uint8_t training_seq_y[38] = { 1,1, 0,0, 0,0, 0,1, 1,0, 0,1, 1,1, 0,0, 1,1, 1,0, 1,0, 0,1, 1,1, 0,0, 0,0, 0,1, 1,0, 0,1, 1,1 };
    uint8_t tsfind_buffer[45];
    bool tsfound = false;
    int symsbeforeexpire = 0;

    char hostname[1024];
    int port = 8355;

    std::shared_ptr<net::Socket> conn;

    // ------------------------------------------------------------------
    // Data export (LIP positions only, TCP to Django map)
    // ------------------------------------------------------------------
    TetraTcpSender tcpSender;
    char data_host[256];
    int data_port = 10100;
    bool tcp_enabled = false;   // bound to the "Enable TCP" checkbox

    // ------------------------------------------------------------------
    // In-memory history of decoded events, displayed in the floating
    // "TETRA Data" window (calls / SDS / status — but NOT LIP positions:
    // those go straight to the map).
    // ------------------------------------------------------------------
    struct CallEvent {
        std::string ts;            // ISO8601 timestamp
        std::string kind;          // "setup", "connect", "release", "tx_ceased", ...
        int caller = 0;            // caller SSI (0 if unknown)
        int callee = 0;            // callee SSI (0 if unknown)
        int call_id = 0;
        int cause = 0;
        int slot = 0;
        std::string call_type;     // "individual" / "group" / "unknown"
    };

    struct SdsEvent {
        std::string ts;
        int src = 0;
        int dst = 0;
        int proto = 0;
        std::string text;
    };

    struct StatusEvent {
        std::string ts;
        int src = 0;
        int dst = 0;
        int code = 0;
    };

    static constexpr size_t MAX_HISTORY = 500;
    std::mutex historyMtx;
    std::deque<CallEvent>   callHistory;
    std::deque<SdsEvent>    sdsHistory;
    std::deque<StatusEvent> statusHistory;
    bool showDataWindow = false;
};

MOD_EXPORT void _INIT_() {
    std::string root = (std::string)core::args["root"];
    json def = json({});
    config.setPath(root + "/tetra_demodulator_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new TetraDemodulatorModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (TetraDemodulatorModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
