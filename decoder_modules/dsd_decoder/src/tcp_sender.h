#pragma once
#include <utils/net.h>
#include <utils/flog.h>
#include <string>
#include <deque>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <chrono>

// Non-blocking TCP client that pushes newline-terminated JSON lines to the
// Django map collector. Same design as the AIS / ADS-B / APRS modules:
//   * a dedicated worker thread owns the socket so the DSP / parser threads
//     are never blocked by network I/O;
//   * lines are queued and flushed by the worker;
//   * the connection is (re)established lazily and retried automatically with
//     a backoff, so the map server can come and go without disturbing the
//     decoder.
class TcpLineSender {
public:
    TcpLineSender() {}
    ~TcpLineSender() { stop(); }

    // Reconfigure target. Safe to call while running; the worker will drop the
    // current socket and reconnect to the new host/port on its next cycle.
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
        worker = std::thread(&TcpLineSender::run, this);
    }

    void stop() {
        if (!running) { return; }
        running = false;
        cv.notify_all();
        if (worker.joinable()) { worker.join(); }
        closeSocket();
    }

    // Enqueue a line for sending. A trailing '\n' is appended if missing.
    // Non-blocking: if the queue is saturated (server gone for a long time)
    // the oldest lines are dropped to bound memory.
    void send(const std::string& line) {
        if (!running || !enabled) { return; }
        std::lock_guard<std::mutex> lck(mtx);
        if (queue.size() >= MAX_QUEUE) {
            queue.pop_front();
            dropped++;
        }
        if (!line.empty() && line.back() == '\n') { queue.push_back(line); }
        else { queue.push_back(line + "\n"); }
        cv.notify_all();
    }

    bool isConnected() const { return connected; }
    uint64_t droppedCount() const { return dropped.load(); }

    // Gate: when disabled, the worker idles, no socket is opened and any
    // existing one is closed. The thread itself stays alive so the toggle
    // is instant; we only want to avoid touching the network until the
    // user actually asks for it.
    void setEnabled(bool en) {
        std::lock_guard<std::mutex> lck(mtx);
        if (en != enabled) {
            enabled = en;
            if (!en) {
                // Drop the queue: lines accumulated while disabled would
                // otherwise be flushed in a burst on the next enable.
                queue.clear();
                dirty = true;
            } else {
                dirty = true;
            }
            cv.notify_all();
        }
    }
    bool isEnabled() const { return enabled.load(); }

private:
    void run() {
        while (running) {
            std::string h;
            int p;
            bool en;
            {
                std::unique_lock<std::mutex> lck(mtx);
                // Wait for either: work to do, a config change, or shutdown.
                cv.wait_for(lck, std::chrono::milliseconds(500), [this] {
                    return !running || dirty || (enabled && !queue.empty());
                });
                if (!running) { break; }
                if (dirty) {
                    dirty = false;
                    lck.unlock();
                    closeSocket();
                    lck.lock();
                }
                h  = host;
                p  = port;
                en = enabled;
            }

            // While disabled, the worker idles. No socket, no traffic, no
            // connection attempt — even when host/port are set.
            if (!en) {
                connected = false;
                continue;
            }

            if (h.empty() || p <= 0) {
                connected = false;
                continue;
            }

            // Ensure we have a live socket.
            if (!sock || !sock->isOpen()) {
                connected = false;
                // Backoff so we don't hammer a dead server.
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
                        flog::info("dsd_decoder: LRRP TCP connected to {}:{}", h, p);
                    }
                }
                catch (const std::exception& e) {
                    connected = false;
                    sock.reset();
                    continue;
                }
            }

            // Flush queued lines.
            std::deque<std::string> batch;
            {
                std::lock_guard<std::mutex> lck(mtx);
                batch.swap(queue);
            }
            for (auto& line : batch) {
                if (!sock || !sock->isOpen()) {
                    // Lost the socket mid-batch: requeue the rest and reconnect.
                    std::lock_guard<std::mutex> lck(mtx);
                    queue.insert(queue.begin(), line);
                    connected = false;
                    break;
                }
                try {
                    sock->sendstr(line);
                }
                catch (const std::exception&) {
                    std::lock_guard<std::mutex> lck(mtx);
                    queue.insert(queue.begin(), line);
                    connected = false;
                    closeSocket();
                    break;
                }
            }
        }
    }

    void closeSocket() {
        if (sock) {
            try { sock->close(); } catch (...) {}
            sock.reset();
        }
        connected = false;
    }

    static constexpr size_t MAX_QUEUE = 2000;

    std::string host;
    int         port = 0;
    bool        dirty = false;

    std::shared_ptr<net::Socket> sock;
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<bool> enabled{false};
    std::atomic<bool> connected{false};
    std::atomic<uint64_t> dropped{0};

    std::deque<std::string> queue;
    std::mutex mtx;
    std::condition_variable cv;
    std::chrono::steady_clock::time_point lastConnectAttempt{};
};
