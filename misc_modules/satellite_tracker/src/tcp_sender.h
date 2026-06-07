/**
 * @file tcp_sender.h
 * @brief Non-blocking TCP JSON-line sender for the SDR++ Map Django backend.
 *
 * Identical design to the AIS / ADS-B / APRS / Radiosonde / Cospas-Sarsat
 * modules:
 *   * a dedicated worker thread owns the socket so the tracking/GUI threads are
 *     never blocked by network I/O;
 *   * lines are queued (bounded, oldest-dropped) and flushed by the worker;
 *   * the connection is (re)established lazily and retried automatically with a
 *     short backoff, so the map server can come and go freely.
 *
 * Uses only the net:: API from sdrpp_core; no external dependencies.
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
#include <cstring>
#include <memory>

class TcpLineSender {
public:
    TcpLineSender() {
        std::strncpy(host, "127.0.0.1", sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
    }

    ~TcpLineSender() { stop(); }

    void setTarget(const char* newHost, int newPort) {
        std::lock_guard<std::mutex> lck(cfgMtx);
        if (newHost) {
            std::strncpy(host, newHost, sizeof(host) - 1);
            host[sizeof(host) - 1] = '\0';
        }
        if (newPort > 0 && newPort < 65536) { port = newPort; }
        forceReconnect = true;
    }

    void start() {
        if (running.exchange(true)) { return; }
        worker = std::thread(&TcpLineSender::run, this);
    }

    // Non-blocking stop: signal the worker and detach a reaper so the GUI
    // thread never stalls on a pending connect/send.
    void stop() {
        if (!running.exchange(false)) { return; }
        cv.notify_all();
        if (worker.joinable()) {
            std::thread reaper([w = std::move(worker)]() mutable {
                if (w.joinable()) { w.join(); }
            });
            reaper.detach();
        }
    }

    bool isConnected() const { return connected.load(); }
    bool isRunning() const { return running.load(); }

    void send(const std::string& line) {
        if (!running.load()) { return; }
        {
            std::lock_guard<std::mutex> lck(qMtx);
            if (queue.size() >= MAX_QUEUE) { queue.pop(); } // drop oldest
            queue.push(line);
        }
        cv.notify_one();
    }

private:
    void run() {
        while (running.load()) {
            // (Re)connect
            if (!conn || !conn->isOpen() || forceReconnect.exchange(false)) {
                connected = false;
                if (conn) { conn->close(); conn.reset(); }

                char h[256];
                int p;
                {
                    std::lock_guard<std::mutex> lck(cfgMtx);
                    std::strncpy(h, host, sizeof(h));
                    h[sizeof(h) - 1] = '\0';
                    p = port;
                }
                try {
                    conn = net::connect(h, (uint16_t)p);
                }
                catch (const std::exception&) {
                    conn.reset();
                }
                if (!conn || !conn->isOpen()) {
                    // backoff
                    std::unique_lock<std::mutex> lck(qMtx);
                    cv.wait_for(lck, std::chrono::seconds(5),
                                [&] { return !running.load() || forceReconnect.load(); });
                    continue;
                }
                connected = true;
            }

            // Drain queue
            std::string line;
            {
                std::unique_lock<std::mutex> lck(qMtx);
                cv.wait(lck, [&] { return !running.load() || !queue.empty() || forceReconnect.load(); });
                if (!running.load()) { break; }
                if (forceReconnect.load()) { continue; }
                if (queue.empty()) { continue; }
                line = std::move(queue.front());
                queue.pop();
            }

            line.push_back('\n');
            try {
                int wr = conn->send((const uint8_t*)line.data(), line.size());
                if (wr <= 0) {
                    connected = false;
                    if (conn) { conn->close(); conn.reset(); }
                }
            }
            catch (const std::exception&) {
                connected = false;
                if (conn) { conn->close(); conn.reset(); }
            }
        }

        connected = false;
        if (conn) { conn->close(); conn.reset(); }
    }

    static constexpr size_t MAX_QUEUE = 1024;

    char host[256];
    int  port = 10100;

    std::atomic<bool> running{ false };
    std::atomic<bool> connected{ false };
    std::atomic<bool> forceReconnect{ false };

    std::shared_ptr<net::Socket> conn;
    std::thread worker;

    std::mutex qMtx;
    std::mutex cfgMtx;
    std::condition_variable cv;
    std::queue<std::string> queue;
};
