#pragma once
#include <utils/net.h>
#include <utils/flog.h>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <chrono>
#include <memory>

// Non-blocking rigctl client used by Voice-Channel instances of the module.
//
// In our split CC/VC trunking setup, the CC instance hosts a small rigctl
// server (see rigctl_internal.h) and broadcasts every "F <freq>" command it
// receives from dsd-fme to all connected clients. A VC instance is one of
// those clients: it sits idle on its own VFO until a freq arrives, then
// retunes there to follow the voice.
//
// We don't *send* anything in this direction (CC -> VC is one-way). We just
// read incoming lines and feed F-typed events to the module via callback.
// Auto-reconnect with a 3s backoff handles the "VC started before CC" case
// and survives the CC restarting mid-session.
class RigctlClient {
public:
    // Fires on every "F <freq>" we receive from the server (runs on the
    // worker thread; the module must take its own locks when needed).
    std::function<void(double)> onSetFreq;

    void configure(const std::string& host, int port) {
        std::lock_guard<std::mutex> lck(mtx);
        if (host != this->host || port != this->port) {
            this->host = host;
            this->port = port;
            dirty = true;
            cv.notify_all();
        }
    }

    void start() {
        if (running) { return; }
        running = true;
        worker = std::thread(&RigctlClient::run, this);
    }

    void stop() {
        if (!running) { return; }
        running = false;
        cv.notify_all();
        closeSocket();
        if (worker.joinable()) { worker.join(); }
    }

    void setEnabled(bool en) {
        std::lock_guard<std::mutex> lck(mtx);
        if (en != enabled) {
            enabled = en;
            dirty = true;
            cv.notify_all();
        }
    }

    bool isEnabled()   const { return enabled.load(); }
    bool isConnected() const { return connected.load(); }

private:
    void run() {
        while (running) {
            // Snapshot config; sleep on the cv ONLY when disabled or
            // misconfigured. When connected we just loop on recvline which
            // has its own timeout — we still come back every 500 ms to
            // check running/dirty.
            std::string h;
            int p;
            bool en;
            bool wasDirty;
            {
                std::unique_lock<std::mutex> lck(mtx);
                if (!enabled || host.empty() || port <= 0) {
                    // Idle: wait for something to change.
                    cv.wait_for(lck, std::chrono::milliseconds(500), [this] {
                        return !running || dirty || (enabled && !host.empty() && port > 0);
                    });
                }
                if (!running) { break; }
                wasDirty = dirty;
                dirty    = false;
                h        = host;
                p        = port;
                en       = enabled;
            }

            if (wasDirty) { closeSocket(); }
            if (!en) { connected = false; continue; }
            if (h.empty() || p <= 0) { connected = false; continue; }

            // Connect if needed, with backoff.
            if (!sock || !sock->isOpen()) {
                connected = false;
                auto now = std::chrono::steady_clock::now();
                if (now - lastConnectAttempt < std::chrono::seconds(3)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    continue;
                }
                lastConnectAttempt = now;
                try {
                    sock = net::connect(h, p);
                    connected = (sock && sock->isOpen());
                    if (connected) {
                        flog::info("dsd_decoder: rigctl client connected to {}:{}", h, p);
                        // Identify ourselves as a VC so the CC's server can
                        // count us separately from dsd-fme (which speaks the
                        // standard rigctl protocol and never sends this).
                        // The server replies "OK\n" which we ignore via the
                        // regular read loop below.
                        try { sock->sendstr("_vc_register\n"); }
                        catch (...) { /* will reconnect */ }
                    }
                }
                catch (const std::exception&) {
                    connected = false;
                    sock.reset();
                    continue;
                }
            }

            // Read lines from the server. recvline has its own 500 ms timeout
            // so we periodically re-check running / dirty without a separate
            // cv.wait — that way successive lines are processed immediately.
            //
            // Note: SDR++'s net API returns 0 from recvline for BOTH a select()
            // timeout AND a peer-closed socket. We distinguish them via
            // isOpen(): on peer-close, recv() internally calls close() so
            // isOpen() turns false; on timeout the socket stays open.
            std::string line;
            int r = sock->recvline(line, 1024, 500);
            if (r == 0) {
                if (!sock->isOpen()) {
                    // Real disconnection — reconnect on next iteration.
                    connected = false;
                    closeSocket();
                }
                // else: just no data in the last 500 ms, keep waiting.
                continue;
            }
            if (r < 0) { continue; }

            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }
            handle(line);
        }
    }

    void handle(const std::string& line) {
        if (line.empty()) { return; }
        // We only care about freq-change commands. Both rigctl short ("F <hz>")
        // and long ("\set_freq <hz>") forms are accepted. Anything else (RPRT
        // replies from us, mode replies, etc.) is ignored.
        size_t sp = line.find(' ');
        if (sp == std::string::npos) { return; }
        std::string cmd = line.substr(0, sp);
        std::string arg = line.substr(sp + 1);
        if (cmd != "F" && cmd != "\\set_freq") { return; }

        double f;
        try { f = std::stod(arg); }
        catch (...) { return; }
        if (f <= 0.0) { return; }
        if (onSetFreq) { onSetFreq(f); }
    }

    void closeSocket() {
        if (sock) {
            try { sock->close(); } catch (...) {}
            sock.reset();
        }
        connected = false;
    }

    std::string host;
    int         port = 0;
    bool        dirty = false;

    std::shared_ptr<net::Socket> sock;
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<bool> enabled{false};
    std::atomic<bool> connected{false};

    std::mutex mtx;
    std::condition_variable cv;
    std::chrono::steady_clock::time_point lastConnectAttempt{};
};
