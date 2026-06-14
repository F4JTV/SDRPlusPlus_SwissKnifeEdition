/*
 * tcp_sender.h - Non-blocking newline-delimited JSON line sender.
 *
 * Shared convention across these SDR++ decoder modules: a dedicated worker
 * thread owns the socket, the GUI thread only enqueues lines, the queue is
 * bounded (oldest dropped on overflow), and the connection auto-reconnects
 * with a fixed back-off. Uses only the SDR++ net:: API (utils/net.h) so there
 * is no external dependency.
 *
 * Output is consumed by the Django "SDR Map" backend, which expects one JSON
 * object per line with keys: name, date, time, lat, lon, type, speed, info.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <utils/net.h>

class TcpLineSender {
public:
    TcpLineSender() = default;
    ~TcpLineSender() { stop(); }

    // Start the worker thread targeting host:port. Safe to call when already
    // running with the same target (no-op); restarts if the target changed.
    void start(const std::string& host, int port) {
        std::lock_guard<std::mutex> lck(stateMtx);
        if (running && host == this->host && port == this->port) return;
        stopLocked();
        this->host = host;
        this->port = port;
        running = true;
        stopFlag = false;
        worker = std::thread(&TcpLineSender::run, this);
    }

    void stop() {
        std::lock_guard<std::mutex> lck(stateMtx);
        stopLocked();
    }

    bool isRunning() const { return running.load(); }
    bool isConnected() const { return connected.load(); }

    // Enqueue one line (a newline is appended if missing). Non-blocking; if the
    // queue is full the oldest entry is dropped.
    void send(const std::string& line) {
        if (!running) return;
        std::lock_guard<std::mutex> lck(queueMtx);
        if (queue.size() >= kMaxQueue) queue.pop_front();
        if (!line.empty() && line.back() == '\n') queue.push_back(line);
        else queue.push_back(line + "\n");
        queueCv.notify_one();
    }

private:
    static constexpr std::size_t kMaxQueue = 1024;

    void stopLocked() {
        if (!running) return;
        {
            std::lock_guard<std::mutex> qlck(queueMtx);
            stopFlag = true;
            queueCv.notify_all();
        }
        if (worker.joinable()) worker.join();
        running = false;
        connected = false;
        queue.clear();
    }

    void run() {
        using namespace std::chrono_literals;
        while (true) {
            {
                std::lock_guard<std::mutex> qlck(queueMtx);
                if (stopFlag) break;
            }

            std::shared_ptr<net::Socket> sock;
            try {
                sock = net::connect(host, port);
            } catch (...) {
                sock = nullptr;
            }

            if (!sock) {
                connected = false;
                // back off 5 s, but wake early on stop
                std::unique_lock<std::mutex> qlck(queueMtx);
                queueCv.wait_for(qlck, 5s, [&] { return stopFlag; });
                if (stopFlag) break;
                continue;
            }

            connected = true;
            while (true) {
                std::string line;
                {
                    std::unique_lock<std::mutex> qlck(queueMtx);
                    queueCv.wait(qlck, [&] { return stopFlag || !queue.empty(); });
                    if (stopFlag) break;
                    line = std::move(queue.front());
                    queue.pop_front();
                }
                int n = sock->sendstr(line);
                if (n <= 0) break;  // broken pipe -> reconnect
            }

            connected = false;
            try { if (sock) sock->close(); } catch (...) {}
            {
                std::lock_guard<std::mutex> qlck(queueMtx);
                if (stopFlag) break;
            }
        }
        connected = false;
    }

    std::string host = "127.0.0.1";
    int port = 10100;

    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<bool> connected{false};
    bool stopFlag = false;

    std::mutex stateMtx;
    std::mutex queueMtx;
    std::condition_variable queueCv;
    std::deque<std::string> queue;
};
