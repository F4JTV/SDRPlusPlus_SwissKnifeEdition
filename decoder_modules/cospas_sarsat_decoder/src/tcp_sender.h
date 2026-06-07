/**
 * @file tcp_sender.h
 * @brief Non-blocking TCP JSON-line sender for SDR++ Map Django backend
 *
 * Same design as the AIS / ADS-B / APRS / Radiosonde modules:
 *   * a dedicated worker thread owns the socket so the DSP / decoder threads
 *     are never blocked by network I/O;
 *   * lines are queued and flushed by the worker;
 *   * the connection is (re)established lazily and retried automatically with
 *     a short backoff, so the map server can come and go without disturbing
 *     the decoder.
 */
#pragma once

#include <utils/net.h>
#include <utils/flog.h>
#include <string>
#include <queue>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <memory>
#include <cstring>

class TcpLineSender {
public:
    TcpLineSender() {
        std::strncpy(host, "127.0.0.1", sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
    }

    ~TcpLineSender() { stopAndJoin(); }

    // Reconfigure target. Safe to call while running; the worker will drop the
    // current socket and reconnect on its next cycle.
    void setTarget(const char* newHost, int newPort) {
        std::lock_guard<std::mutex> lck(cfgMtx);
        if (newHost) {
            std::strncpy(host, newHost, sizeof(host) - 1);
            host[sizeof(host) - 1] = '\0';
        }
        if (newPort > 0 && newPort < 65536) {
            port = newPort;
        }
        // Force reconnect on next worker iteration.
        forceReconnect.store(true);
        // Drop current socket so any blocking call is interrupted.
        std::lock_guard<std::mutex> sl(sockMtx);
        if (sock && sock->isOpen()) { sock->close(); }
        sock.reset();
        queueCv.notify_all();
    }

    bool isRunning() const { return running.load(); }
    bool isConnected() const { return connected.load(); }

    // Push a line for the worker. A trailing '\n' is appended if missing.
    // Drops the line silently if the queue is full or the sender is stopped,
    // so the caller (a DSP thread) is never blocked.
    void push(const std::string& line) {
        if (!running.load()) { return; }
        std::lock_guard<std::mutex> lck(queueMtx);
        if (queue.size() >= MAX_QUEUE) {
            // Drop the oldest to keep memory bounded.
            queue.pop();
        }
        if (!line.empty() && line.back() == '\n') {
            queue.push(line);
        } else {
            queue.push(line + "\n");
        }
        queueCv.notify_one();
    }

    void start() {
        if (running.exchange(true)) { return; }
        std::thread t(&TcpLineSender::run, this);
        worker = std::move(t);
    }

    // Non-blocking stop: never join() from the GUI thread, because the worker
    // can be parked inside a synchronous net::connect() (potentially blocking
    // for tens of seconds when the collector is unreachable) or a sendstr()
    // waiting on the kernel. Joining there would freeze the SDR++ UI.
    //
    // Strategy:
    //   1. clear the running flag,
    //   2. close the socket (this aborts any in-flight connect/send),
    //   3. signal the queue CV (wakes any wait_for),
    //   4. move the worker into a tiny "reaper" thread that joins it in the
    //      background. The GUI thread never blocks.
    void stop() {
        if (!running.exchange(false)) { return; }
        {
            std::lock_guard<std::mutex> lck(sockMtx);
            if (sock && sock->isOpen()) { sock->close(); }
            sock.reset();
        }
        connected.store(false);
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

    // Synchronous stop. Only safe from non-GUI threads (destructor uses it).
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
        connected.store(false);
        {
            std::lock_guard<std::mutex> lck(queueMtx);
            std::queue<std::string> empty;
            std::swap(queue, empty);
        }
        queueCv.notify_all();
        if (worker.joinable()) { worker.join(); }
    }

private:
    void run() {
        using namespace std::chrono;
        while (running.load()) {
            // (Re)connect if needed
            if (!isSockOpen() || forceReconnect.exchange(false)) {
                tryConnect();
                if (!isSockOpen()) {
                    // Interruptible backoff before next attempt.
                    std::unique_lock<std::mutex> lck(queueMtx);
                    queueCv.wait_for(lck, milliseconds(BACKOFF_MS),
                                     [this]{ return !running.load() || forceReconnect.load(); });
                    continue;
                }
            }

            // Wait for a line or shutdown signal.
            std::string line;
            {
                std::unique_lock<std::mutex> lck(queueMtx);
                queueCv.wait(lck, [this]{ return !running.load() || !queue.empty() || forceReconnect.load(); });
                if (!running.load()) { break; }
                if (queue.empty()) { continue; }
                line = std::move(queue.front());
                queue.pop();
            }

            // Send (blocking, but interruptible by sock->close()).
            bool ok = false;
            {
                std::lock_guard<std::mutex> lck(sockMtx);
                if (sock && sock->isOpen()) {
                    try {
                        int n = sock->sendstr(line);
                        ok = (n >= 0) || sock->isOpen();
                    } catch (...) {
                        ok = false;
                    }
                }
            }
            if (!ok) {
                std::lock_guard<std::mutex> lck(sockMtx);
                if (sock) {
                    if (sock->isOpen()) { sock->close(); }
                    sock.reset();
                }
                connected.store(false);
            }
        }
    }

    void tryConnect() {
        std::string h;
        int p;
        {
            std::lock_guard<std::mutex> lck(cfgMtx);
            h = host;
            p = port;
        }
        try {
            auto s = net::connect(h, p);
            std::lock_guard<std::mutex> lck(sockMtx);
            sock = s;
            connected.store(sock && sock->isOpen());
            if (connected.load()) {
                flog::info("TCP sender connected to {}:{}", h, p);
            }
        } catch (const std::exception& e) {
            connected.store(false);
            flog::debug("TCP sender connect failed: {}", e.what());
        } catch (...) {
            connected.store(false);
        }
    }

    bool isSockOpen() {
        std::lock_guard<std::mutex> lck(sockMtx);
        return sock && sock->isOpen();
    }

    static constexpr size_t MAX_QUEUE = 1024;
    static constexpr int BACKOFF_MS = 5000;

    // Config (host + port). Defaults set in constructor.
    std::mutex cfgMtx;
    char host[256];
    int port = 10100;

    // Worker / state
    std::atomic<bool> running{false};
    std::atomic<bool> connected{false};
    std::atomic<bool> forceReconnect{false};
    std::thread worker;

    // Outbound queue
    std::mutex queueMtx;
    std::condition_variable queueCv;
    std::queue<std::string> queue;

    // Socket
    std::mutex sockMtx;
    std::shared_ptr<net::Socket> sock;
};
