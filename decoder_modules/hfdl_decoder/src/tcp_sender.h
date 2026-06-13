#pragma once
#include <string>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <utils/net.h>
#include <utils/flog.h>

// Non-blocking TCP client used to forward decoded contacts to an external
// collector (e.g. a Django map server). Lines are queued and flushed by a
// dedicated worker thread that reconnects automatically with a back-off.
//
// One JSON object per line, terminated by '\n'.
class TCPSender {
public:
    ~TCPSender() { stop(); }

    void start(const std::string& host, int port) {
        stop();
        _host = host;
        _port = port;
        _run = true;
        _worker = std::thread(&TCPSender::workerLoop, this);
    }

    void stop() {
        _run = false;
        _cv.notify_all();
        if (_worker.joinable()) { _worker.join(); }
        std::lock_guard<std::mutex> lck(_sockMtx);
        if (_sock && _sock->isOpen()) { _sock->close(); }
        _sock.reset();
        {
            std::lock_guard<std::mutex> qlck(_qMtx);
            _queue.clear();
        }
    }

    bool isRunning() const { return _run.load(); }
    bool isConnected() const { return _connected.load(); }

    // Enqueue a line for transmission (a '\n' is appended if missing).
    void send(const std::string& line) {
        if (!_run) { return; }
        std::string l = line;
        if (l.empty() || l.back() != '\n') { l.push_back('\n'); }
        {
            std::lock_guard<std::mutex> lck(_qMtx);
            // Bound the queue so a dead collector can't grow memory forever.
            if (_queue.size() > 10000) { _queue.pop_front(); }
            _queue.push_back(std::move(l));
        }
        _cv.notify_one();
    }

private:
    void workerLoop() {
        int backoffMs = 500;
        while (_run) {
            // (Re)connect if needed.
            if (!_connected) {
                std::shared_ptr<net::Socket> s;
                try {
                    s = net::connect(_host, _port);
                } catch (const std::exception& e) {
                    flog::warn("[HFDL-TCP] connect failed: {}", e.what());
                }
                if (s && s->isOpen()) {
                    {
                        std::lock_guard<std::mutex> lck(_sockMtx);
                        _sock = s;
                    }
                    _connected = true;
                    backoffMs = 500;
                    flog::info("[HFDL-TCP] connected to {}:{}", _host, _port);
                } else {
                    // Wait before retrying (capped exponential back-off).
                    std::unique_lock<std::mutex> lck(_qMtx);
                    _cv.wait_for(lck, std::chrono::milliseconds(backoffMs),
                                 [this] { return !_run.load(); });
                    backoffMs = std::min(backoffMs * 2, 8000);
                    continue;
                }
            }

            // Drain the queue.
            std::string line;
            {
                std::unique_lock<std::mutex> lck(_qMtx);
                _cv.wait_for(lck, std::chrono::milliseconds(1000),
                             [this] { return !_queue.empty() || !_run.load(); });
                if (!_run) { break; }
                if (_queue.empty()) { continue; }
                line = std::move(_queue.front());
                _queue.pop_front();
            }

            // Send it. On failure, drop the connection and re-queue the line.
            bool ok = false;
            {
                std::lock_guard<std::mutex> lck(_sockMtx);
                if (_sock && _sock->isOpen()) {
                    int n = _sock->sendstr(line);
                    ok = (n > 0);
                }
            }
            if (!ok) {
                flog::warn("[HFDL-TCP] send failed, will reconnect");
                {
                    std::lock_guard<std::mutex> lck(_sockMtx);
                    if (_sock && _sock->isOpen()) { _sock->close(); }
                    _sock.reset();
                }
                _connected = false;
                std::lock_guard<std::mutex> qlck(_qMtx);
                _queue.push_front(line); // retry after reconnect
            }
        }

        std::lock_guard<std::mutex> lck(_sockMtx);
        if (_sock && _sock->isOpen()) { _sock->close(); }
        _sock.reset();
        _connected = false;
    }

    std::string _host = "127.0.0.1";
    int _port = 10100;

    std::atomic<bool> _run{false};
    std::atomic<bool> _connected{false};
    std::thread _worker;

    std::mutex _qMtx;
    std::condition_variable _cv;
    std::deque<std::string> _queue;

    std::mutex _sockMtx;
    std::shared_ptr<net::Socket> _sock;
};
