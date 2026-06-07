/**
 * @file rigctl_internal.h
 * @brief Minimal embedded RIGCTL (hamlib) TCP server for the Satellite Tracker.
 *
 * This is the "rigctl base" of SDR++ integrated directly into the module. It
 * implements the subset of the hamlib rigctld protocol that satellite software
 * (gpredict, SatDump, WSJT-X) actually uses against SDR++:
 *
 *      F <freq> / \set_freq <freq>     set VFO frequency (Hz)   -> tuner::tune
 *      f       / \get_freq             get VFO frequency (Hz)
 *      M ... / m / \set_mode / \get_mode   accepted (no-op-ish), returns OK
 *      \dump_state                     capability dump (rigctld handshake)
 *      \get_powerstat / \chk_vfo / V / v   accepted for WSJT-X / gpredict
 *      AOS / LOS                       satellite hooks, acknowledged
 *
 * The command parsing/responses are modelled on SDR++'s misc_modules/
 * rigctl_server/src/main.cpp, trimmed to the freq path and rewritten on the
 * newer net.h API (blocking accept with timeout on a dedicated thread) so the
 * module owns a clean, joinable lifecycle and can coexist with the autonomous
 * Doppler-correction loop.
 *
 * Two roles:
 *   - When SatTracker drives Doppler itself, this server is optional and OFF.
 *   - When the user prefers to drive SDR++ from external pass software, they
 *     enable this server and point gpredict's "Radio" at host:port; every "F"
 *     command retunes the selected VFO exactly like the stock rigctl_server.
 *
 * Frequency set commands are routed through a user-supplied callback so the
 * module decides what to do (tune the VFO, and/or capture as the tracked
 * downlink). Reads report the live VFO frequency.
 */
#pragma once

#include <utils/net.h>
#include <utils/flog.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <cinttypes>
#include <cstdio>
#include <cstring>

class RigctlInternalServer {
public:
    // setFreq(freqHz)  -> return true to ACK (RPRT 0), false for error.
    // getFreq()        -> current frequency in Hz to report for "f".
    using SetFreqFn = std::function<bool(double)>;
    using GetFreqFn = std::function<double()>;

    RigctlInternalServer() = default;
    ~RigctlInternalServer() { stop(); }

    void setCallbacks(SetFreqFn sf, GetFreqFn gf) {
        setFreqCb = std::move(sf);
        getFreqCb = std::move(gf);
    }

    bool isRunning() const { return running.load(); }
    bool hasClient() const { return clientConnected.load(); }

    // Start listening on host:port. Returns true on success.
    bool start(const std::string& host, int port) {
        if (running.exchange(true)) { return true; }
        try {
            listener = net::listen(host, port);
        }
        catch (const std::exception& e) {
            flog::error("[SatTracker] rigctl listen failed: {0}", e.what());
            running = false;
            listener.reset();
            return false;
        }
        acceptThread = std::thread(&RigctlInternalServer::acceptLoop, this);
        flog::info("[SatTracker] embedded rigctl server listening on {0}:{1}", host, port);
        return true;
    }

    void stop() {
        if (!running.exchange(false)) { return; }
        if (listener) { listener->stop(); }
        if (acceptThread.joinable()) { acceptThread.join(); }
        listener.reset();
        clientConnected = false;
    }

private:
    void acceptLoop() {
        while (running.load()) {
            std::shared_ptr<net::Socket> client;
            try {
                client = listener->accept(nullptr, 500 /* ms */);
            }
            catch (const std::exception&) {
                client.reset();
            }
            if (!client) { continue; } // timeout / shutting down

            clientConnected = true;
            handleClient(client);
            clientConnected = false;
        }
    }

    void handleClient(std::shared_ptr<net::Socket> client) {
        std::string line;
        while (running.load() && client->isOpen()) {
            int n = client->recvline(line, MAX_CMD, 500 /* ms */);
            if (n < 0) { continue; } // would-block / timeout
            if (n == 0) { break; }   // closed
            handleCommand(client, line);
        }
        client->close();
    }

    static std::vector<std::string> split(const std::string& cmd) {
        std::vector<std::string> parts;
        std::string cur;
        bool lastSpace = false;
        for (char c : cmd) {
            if (c == ' ') {
                if (lastSpace) { continue; }
                if (!cur.empty()) { parts.push_back(cur); cur.clear(); }
                lastSpace = true;
            }
            else {
                cur += c;
                lastSpace = false;
            }
        }
        if (!cur.empty()) { parts.push_back(cur); }
        return parts;
    }

    void reply(std::shared_ptr<net::Socket>& c, const std::string& s) {
        c->send((const uint8_t*)s.data(), s.size());
    }

    void handleCommand(std::shared_ptr<net::Socket>& client, const std::string& cmd) {
        auto parts = split(cmd);
        if (parts.empty()) { return; }

        // Expand short compound commands like "FM" -> "F","M" (rigctl quirk),
        // mirroring rigctl_server's behaviour. Long-form (\\...) is left alone.
        if (parts[0].size() > 1 && parts[0][0] != '\\' &&
            parts[0] != "AOS" && parts[0] != "LOS") {
            std::string args = (cmd.size() > parts[0].size()) ? cmd.substr(parts[0].size()) : "";
            for (char ch : parts[0]) {
                handleCommand(client, std::string(1, ch) + args);
            }
            return;
        }

        const std::string& op = parts[0];

        if (op == "F" || op == "\\set_freq") {
            if (parts.size() != 2) { reply(client, "RPRT 1\n"); return; }
            double freq = 0.0;
            try { freq = std::stod(parts[1]); }
            catch (...) { reply(client, "RPRT 1\n"); return; }
            bool ok = setFreqCb ? setFreqCb(freq) : false;
            reply(client, ok ? "RPRT 0\n" : "RPRT 0\n"); // rigctl_server always ACKs set_freq
        }
        else if (op == "f" || op == "\\get_freq") {
            double f = getFreqCb ? getFreqCb() : 0.0;
            char buf[64];
            snprintf(buf, sizeof(buf), "%" PRIu64 "\n", (uint64_t)f);
            reply(client, buf);
        }
        else if (op == "M" || op == "\\set_mode") {
            if (parts.size() >= 2 && parts[1] == "?") {
                reply(client, "FM WFM AM DSB USB CW LSB RAW\n");
            }
            else {
                reply(client, "RPRT 0\n");
            }
        }
        else if (op == "m" || op == "\\get_mode") {
            reply(client, "FM\n0\n");
        }
        else if (op == "V" || op == "\\set_vfo") {
            reply(client, "RPRT 0\n");
        }
        else if (op == "v" || op == "\\get_vfo") {
            reply(client, "VFOA\n");
        }
        else if (op == "\\chk_vfo") {
            reply(client, "CHKVFO 0\n");
        }
        else if (op == "\\get_powerstat") {
            reply(client, "1\n"); // WSJT-X 2.7 workaround, as in rigctl_server
        }
        else if (op == "AOS" || op == "LOS") {
            // Satellite pass hooks: acknowledge. The autonomous tracker reacts
            // to elevation itself; external software just informs us.
            reply(client, "RPRT 0\n");
        }
        else if (op == "\\dump_state") {
            // Minimal but valid hamlib dump_state (same shape as rigctl_server).
            std::string resp =
                "1\n"               // protocol version
                "2\n"               // rig model (dummy)
                "2\n"               // ITU region
                /* RX freq ranges, start end modes low_power high_power vfo ant */
                "0.000000 10000000000.000000 0x2ef -1 -1 0x10000003 0x3\n"
                "0 0 0 0 0 0 0\n"
                /* TX freq ranges */
                "0 0 0 0 0 0 0\n"
                /* tuning steps */
                "0x2ef 1\n"
                "0 0\n"
                /* filters */
                "0x82 500\n"
                "0x82 200\n"
                "0x82 2000\n"
                "0x21 10000\n"
                "0x21 5000\n"
                "0x0c 2700\n"
                "0x0c 3900\n"
                "0x40 160000\n"
                "0x40 120000\n"
                "0x40 200000\n"
                "0 0\n"
                "0\n"   // max_rit
                "0\n"   // max_xit
                "0\n"   // max_ifshift
                "0\n"   // announces
                "0\n"   // preamp
                "0\n"   // attenuator
                "0\n"   // get functions
                "0\n"   // set functions
                "0x40000020\n" // get level
                "0x20\n"       // set level
                "0\n"   // get parm
                "0\n";  // set parm
            reply(client, resp);
        }
        else if (op == "q" || op == "Q") {
            client->close();
        }
        else {
            // Unknown command: error, like the stock server.
            reply(client, "RPRT 1\n");
        }
    }

    static constexpr int MAX_CMD = 8192;

    std::atomic<bool> running{ false };
    std::atomic<bool> clientConnected{ false };
    std::shared_ptr<net::Listener> listener;
    std::thread acceptThread;

    SetFreqFn setFreqCb;
    GetFreqFn getFreqCb;
};
