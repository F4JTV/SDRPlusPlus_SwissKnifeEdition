#pragma once
#include <utils/net.h>
#include <utils/flog.h>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <condition_variable>

// ---------------------------------------------------------------------------
//  TCP sender for decoded APRS records
//
//  Connects (as a TCP *client*) to an external collector — e.g. the Django map
//  project's "listen_aprs" management command — and pushes one JSON object per
//  line, exactly mirroring the schema used by the AIS decoder module:
//
//    {"name":"F4JTV-9","date":"2026-05-25","time":"12:34:56",
//     "lat":43.30000,"lon":5.36667,"type":"APRS","speed":36.0,
//     "info":"MIC-E crs=88 sym=/> via WIDE1-1 — ADRASEC 06 mobile"}
//
//  - speed is null when not provided by the station.
//  - Only positioned records are enqueued (handled by the caller).
//  - A dedicated worker thread owns the socket; the GUI thread only enqueues,
//    so RF/decoding is never blocked by network stalls. Auto-reconnects.
// ---------------------------------------------------------------------------

class TCPSender {
public:
    TCPSender() {}
    ~TCPSender() { stopAndJoin(); }

    void configure(const std::string& host, int port) {
        std::lock_guard<std::mutex> lck(cfgMtx);
        _host = host;
        _port = port;
    }

    void start() {
        if (running.exchange(true)) { return; }
        std::thread t(&TCPSender::run, this);
        worker = std::move(t);
    }

    // Non-blocking stop: we must never join() from the GUI thread, because the
    // worker can be parked inside a synchronous net::connect() (which can take
    // tens of seconds when the collector is unreachable) or a sendstr() waiting
    // on the kernel. Joining would freeze the SDR++ UI.
    //
    // Strategy:
    //   1. clear the running flag,
    //   2. close the socket (this aborts any in-flight connect/send),
    //   3. signal the queue CV (wakes any wait_for),
    //   4. move the worker into a tiny "reaper" thread that joins it in the
    //      background. The GUI thread never blocks. The reaper itself joins the
    //      worker so its thread of execution is properly cleaned up.
    void stop() {
        if (!running.exchange(false)) { return; }
        {
            std::lock_guard<std::mutex> lck(sockMtx);
            if (sock && sock->isOpen()) { sock->close(); }
            sock.reset();
        }
        {
            std::lock_guard<std::mutex> lck(queueMtx);
            std::queue<std::string> empty;
            std::swap(queue, empty);
        }
        queueCv.notify_all();
        if (worker.joinable()) {
            std::thread reaper([t = std::move(worker)]() mutable {
                if (t.joinable()) { t.join(); }
            });
            reaper.detach();
        }
    }

    // Synchronous stop used by the destructor only: we want the worker to be
    // fully reaped before our members go away. Caller MUST NOT be the GUI
    // thread (it's only called from ~TCPSender at module shutdown).
    void stopAndJoin() {
        if (!running.exchange(false)) {
            if (worker.joinable()) { worker.join(); }
            return;
        }
        {
            std::lock_guard<std::mutex> lck(sockMtx);
            if (sock && sock->isOpen()) { sock->close(); }
            sock.reset();
        }
        {
            std::lock_guard<std::mutex> lck(queueMtx);
            std::queue<std::string> empty;
            std::swap(queue, empty);
        }
        queueCv.notify_all();
        if (worker.joinable()) { worker.join(); }
    }

    bool isRunning() { return running.load(); }
    bool isConnected() { return connected.load(); }
    uint64_t getSent() { return sentCount.load(); }
    uint64_t getDropped() { return droppedCount.load(); }

    // Enqueue a ready-to-send JSON line (terminated with '\n' added here).
    void send(const std::string& jsonLine) {
        if (!running.load()) { return; }
        std::lock_guard<std::mutex> lck(queueMtx);
        if (queue.size() >= MAX_QUEUE) {
            droppedCount++;
            return; // backpressure: drop oldest-policy could also be used
        }
        queue.push(jsonLine + "\n");
        queueCv.notify_one();
    }

private:
    void run() {
        // Interruptible sleep: returns false if stop() was called, true otherwise.
        auto waitOrStop = [this](int ms) -> bool {
            std::unique_lock<std::mutex> lck(queueMtx);
            queueCv.wait_for(lck, std::chrono::milliseconds(ms),
                             [this] { return !running.load(); });
            return running.load();
        };

        while (running.load()) {
            // (Re)connect if needed
            if (!connected.load()) {
                std::string host; int port;
                {
                    std::lock_guard<std::mutex> lck(cfgMtx);
                    host = _host; port = _port;
                }
                if (host.empty() || port <= 0) {
                    if (!waitOrStop(500)) { break; }
                    continue;
                }
                try {
                    auto s = net::connect(host, port);
                    if (!running.load()) { if (s && s->isOpen()) { s->close(); } break; }
                    if (s && s->isOpen()) {
                        std::lock_guard<std::mutex> lck(sockMtx);
                        sock = s;
                        connected.store(true);
                        flog::info("APRS TCP: connected to {}:{}", host, port);
                    } else {
                        if (!waitOrStop(1000)) { break; }
                        continue;
                    }
                } catch (const std::exception& e) {
                    flog::warn("APRS TCP: connect failed: {}", e.what());
                    if (!waitOrStop(1000)) { break; }
                    continue;
                }
            }

            // Wait for a queued line
            std::string line;
            {
                std::unique_lock<std::mutex> lck(queueMtx);
                queueCv.wait_for(lck, std::chrono::milliseconds(500),
                                 [this] { return !queue.empty() || !running.load(); });
                if (!running.load()) { break; }
                if (queue.empty()) { continue; }
                line = queue.front();
                queue.pop();
            }

            // Send it
            bool ok = false;
            {
                std::lock_guard<std::mutex> lck(sockMtx);
                if (sock && sock->isOpen()) {
                    try {
                        int n = sock->sendstr(line);
                        ok = (n > 0);
                    } catch (...) { ok = false; }
                }
            }
            if (ok) {
                sentCount++;
            } else {
                // Connection lost -> drop socket and reconnect on next loop
                std::lock_guard<std::mutex> lck(sockMtx);
                if (sock) { sock->close(); sock.reset(); }
                connected.store(false);
                droppedCount++;
                flog::warn("APRS TCP: send failed, will reconnect");
            }
        }
        connected.store(false);
    }

    static constexpr size_t MAX_QUEUE = 2000;

    std::string _host = "127.0.0.1";
    int _port = 10111;
    std::mutex cfgMtx;

    std::shared_ptr<net::Socket> sock;
    std::mutex sockMtx;

    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<bool> connected{false};
    std::atomic<uint64_t> sentCount{0};
    std::atomic<uint64_t> droppedCount{0};

    std::queue<std::string> queue;
    std::mutex queueMtx;
    std::condition_variable queueCv;
};
