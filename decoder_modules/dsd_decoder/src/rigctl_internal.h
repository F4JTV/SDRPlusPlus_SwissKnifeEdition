#pragma once
#include <utils/net.h>
#include <utils/flog.h>
#include <string>
#include <sstream>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <memory>
#include <functional>
#include <algorithm>
#include <cstdio>

// Minimal embedded RIGCTL server. Implements the subset of the hamlib rigctl
// TCP protocol that DSD-FME actually exercises:
//
//   F <hz>            / \set_freq <hz>     -> retune (we expose a callback)
//   f                 / \get_freq          -> reply with current VFO freq in Hz
//   M <mode> <pass>   / \set_mode          -> accepted, no-op
//   m                 / \get_mode          -> reply "FM\n<bandwidth>\n"
//   V <vfo>           / \set_vfo           -> accepted, no-op
//   v                 / \get_vfo           -> reply "VFOA\n"
//   \chk_vfo                               -> reply "CHKVFO 0\n"
//   \dump_state                            -> reply with a minimal but valid dump
//   q / Q                                  -> close connection
//
// Unknown commands get "RPRT -11\n" (ENAVAIL), which is the standard hamlib way
// to say "not implemented" without aborting the client.
//
// Cross-platform via SDR++'s net:: utility. POSIX/Windows is handled by the
// net::Listener / net::Socket layer underneath.
class RigctlInternalServer {
public:
    // External hooks. Both run on a client thread, not the UI thread, so the
    // module's implementation must synchronise its state.
    std::function<bool(double)> onSetFreq;       // return true = accepted (RPRT 0)
    std::function<double()>     getCurrentFreq;  // current VFO frequency in Hz
    std::function<double()>     getCurrentBw;    // current VFO bandwidth in Hz

    // Start listening on 127.0.0.1:<port>. Idempotent.
    bool start(int port) {
        if (running) { return true; }
        try {
            listener = net::listen("127.0.0.1", port);
        }
        catch (const std::exception& e) {
            flog::warn("dsd_decoder: rigctl listen on 127.0.0.1:{} failed: {}", port, e.what());
            listener.reset();
            return false;
        }
        this->port = port;
        running = true;
        acceptThread = std::thread(&RigctlInternalServer::acceptLoop, this);
        flog::info("dsd_decoder: internal rigctl server listening on 127.0.0.1:{}", port);
        return true;
    }

    void stop() {
        if (!running) { return; }
        running = false;

        // Unblock the accept thread.
        if (listener) {
            try { listener->stop(); } catch (...) {}
        }
        if (acceptThread.joinable()) { acceptThread.join(); }

        // Close every client socket; their recvline() returns 0 promptly.
        std::vector<std::shared_ptr<net::Socket>> snapshot;
        {
            std::lock_guard<std::mutex> lck(mtx);
            snapshot = clients;
            clients.clear();
        }
        for (auto& s : snapshot) {
            try { s->close(); } catch (...) {}
        }
        for (auto& t : clientThreads) {
            if (t.joinable()) { t.join(); }
        }
        clientThreads.clear();
        listener.reset();
    }

    bool isRunning() const { return running.load(); }
    int  getPort()   const { return port; }
    int  clientCount() {
        std::lock_guard<std::mutex> lck(mtx);
        return (int)clients.size();
    }

    // Push "F <hz>\n" to every connected client. Used by the CC instance to
    // forward grants to all the VC instances listening to us. Best-effort:
    // dead/slow clients are silently dropped instead of stalling the caller.
    void broadcastSetFreq(double f) {
        char line[64];
        snprintf(line, sizeof(line), "F %.0f\n", f);
        std::vector<std::shared_ptr<net::Socket>> snapshot;
        {
            std::lock_guard<std::mutex> lck(mtx);
            snapshot = clients;
        }
        for (auto& s : snapshot) {
            if (!s || !s->isOpen()) { continue; }
            try { s->sendstr(line); }
            catch (...) { /* drop and move on; clientLoop will clean up */ }
        }
    }

private:
    void acceptLoop() {
        while (running) {
            std::shared_ptr<net::Socket> s;
            try {
                s = listener->accept(nullptr, 500); // 500 ms timeout to re-check running
            }
            catch (const std::exception&) {
                break;
            }
            if (!s) { continue; }
            {
                std::lock_guard<std::mutex> lck(mtx);
                clients.push_back(s);
            }
            clientThreads.emplace_back(&RigctlInternalServer::clientLoop, this, s);
        }
    }

    void clientLoop(std::shared_ptr<net::Socket> s) {
        while (running && s->isOpen()) {
            std::string line;
            int r = s->recvline(line, 1024, 500);
            if (r == 0) { break; }           // peer closed
            if (r < 0)  { continue; }         // timeout / would-block

            // Trim trailing CR / spaces.
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }

            std::string reply = handle(line);
            if (reply == "<close>") { break; }
            if (!reply.empty()) {
                try { s->sendstr(reply); }
                catch (...) { break; }
            }
        }
        try { s->close(); } catch (...) {}

        std::lock_guard<std::mutex> lck(mtx);
        clients.erase(std::remove(clients.begin(), clients.end(), s), clients.end());
    }

    std::string handle(const std::string& line) {
        if (line.empty()) { return ""; }

        std::vector<std::string> parts;
        std::istringstream iss(line);
        std::string t;
        while (iss >> t) { parts.push_back(t); }
        if (parts.empty()) { return ""; }
        const std::string& cmd = parts[0];

        if (cmd == "F" || cmd == "\\set_freq") {
            if (parts.size() < 2) { return "RPRT -1\n"; }
            double f;
            try { f = std::stod(parts[1]); } catch (...) { return "RPRT -1\n"; }
            bool ok = onSetFreq ? onSetFreq(f) : false;
            return ok ? "RPRT 0\n" : "RPRT -1\n";
        }
        if (cmd == "f" || cmd == "\\get_freq") {
            double f = getCurrentFreq ? getCurrentFreq() : 0.0;
            char b[64]; snprintf(b, sizeof(b), "%.0f\n", f);
            return b;
        }
        if (cmd == "M" || cmd == "\\set_mode") {
            return "RPRT 0\n";
        }
        if (cmd == "m" || cmd == "\\get_mode") {
            double bw = getCurrentBw ? getCurrentBw() : 12500.0;
            char b[64]; snprintf(b, sizeof(b), "FM\n%.0f\n", bw);
            return b;
        }
        if (cmd == "V" || cmd == "\\set_vfo") { return "RPRT 0\n"; }
        if (cmd == "v" || cmd == "\\get_vfo") { return "VFOA\n"; }
        if (cmd == "\\chk_vfo")               { return "CHKVFO 0\n"; }
        if (cmd == "\\dump_state") {
            // A minimal but valid hamlib dump_state response; covers the
            // generic capabilities checks dsd-fme / rigctl performs on connect.
            return
                "0\n"                                                                    // protocol
                "2\n2\n"                                                                  // rig model, version major
                "150000.000000 30000000000.000000 0x1ff -1 -1 0x10000003 0x3\n"
                "0 0 0 0 0 0 0\n"
                "150000.000000 30000000000.000000 0x1ff -1 -1 0x10000003 0x3\n"
                "0 0 0 0 0 0 0\n"
                "0 0\n"
                "0 0\n"
                "0\n0\n0\n0\n"
                "\n\n0x0\n0x0\n0x0\n0x0\n0x0\n0\n";
        }
        if (cmd == "q" || cmd == "Q") { return "<close>"; }
        return "RPRT -11\n";  // RIG_ENAVAIL
    }

    std::atomic<bool> running{false};
    std::shared_ptr<net::Listener>             listener;
    int                                        port = 4532;
    std::thread                                acceptThread;
    std::vector<std::thread>                   clientThreads;
    std::vector<std::shared_ptr<net::Socket>>  clients;
    std::mutex                                 mtx;
};
