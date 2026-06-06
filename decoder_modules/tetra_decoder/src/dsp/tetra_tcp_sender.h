#pragma once

/* TETRA -> Django map TCP line sender.
 *
 * Same wire format and same non-blocking design as the AIS / ADS-B / APRS /
 * DSD-FME / radiosonde modules:
 *
 *   {"name":"SSI:<ssi>","mmsi":<ssi>,"date":"YYYY-MM-DD","time":"HH:MM:SS",
 *    "lat":<deg>,"lon":<deg>,"type":"TETRA","speed":<kph|null>,
 *    "info":"acc=<m> dir=<deg>"}
 *
 * One JSON line per LIP position fix, ending with '\n'. The module is the
 * TCP client; it connects to the configured host/port and reconnects with
 * exponential backoff if the server goes away. A bounded queue absorbs
 * bursts; on overflow the oldest line is dropped (radio doesn't wait, the
 * map collector does).
 */

#include <utils/net.h>
#include <utils/flog.h>
#include <string>
#include <deque>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <chrono>

class TetraTcpSender {
public:
    TetraTcpSender() {}
    ~TetraTcpSender() { stop(); }

    /* Reconfigure target. Safe to call while running; the worker drops the
     * current socket and reconnects to the new host/port next cycle. */
    void configure(const std::string& h, int p) {
        std::lock_guard<std::mutex> lck(mtx);
        host = h;
        port = p;
        reconnect = true;
        cv.notify_one();
    }

    void start() {
        if (running.load()) return;
        running.store(true);
        worker = std::thread(&TetraTcpSender::loop, this);
    }

    void stop() {
        if (!running.load()) return;
        running.store(false);
        cv.notify_all();
        if (worker.joinable()) worker.join();
        if (sock) { try { sock->close(); } catch (...) {} sock.reset(); }
    }

    bool isRunning() const { return running.load(); }
    bool isConnected() const {
        return sock && sock->isOpen();
    }
    uint64_t getSentCount() const { return sent_count.load(); }
    uint64_t getDroppedCount() const { return dropped_count.load(); }

    /* Queue a single JSON line (caller should NOT add a trailing '\n', we
     * append it here). Thread-safe; returns immediately. */
    void send(const std::string& line) {
        if (!running.load()) return;
        std::lock_guard<std::mutex> lck(mtx);
        if (queue.size() >= maxQueue) {
            queue.pop_front();
            dropped_count.fetch_add(1, std::memory_order_relaxed);
        }
        queue.push_back(line + "\n");
        cv.notify_one();
    }

private:
    void loop() {
        int backoff_ms = 1000;
        while (running.load()) {
            /* (Re)connect phase */
            if (!sock || !sock->isOpen() || reconnect) {
                if (sock) { try { sock->close(); } catch (...) {} sock.reset(); }
                std::string h; int p;
                { std::lock_guard<std::mutex> lck(mtx); h = host; p = port; reconnect = false; }
                try {
                    sock = net::connect(h, (uint16_t)p);
                } catch (const std::exception& e) {
                    /* Sleep with periodic wakeup so stop() stays responsive. */
                    std::unique_lock<std::mutex> lck(mtx);
                    cv.wait_for(lck, std::chrono::milliseconds(backoff_ms),
                                [&]{ return !running.load() || reconnect; });
                    backoff_ms = std::min(backoff_ms * 2, 16000);
                    continue;
                }
                backoff_ms = 1000;
            }

            /* Drain phase: pop one line, send it, repeat. */
            std::string line;
            {
                std::unique_lock<std::mutex> lck(mtx);
                cv.wait(lck, [&]{ return !running.load() || reconnect || !queue.empty(); });
                if (!running.load()) break;
                if (reconnect) continue;
                if (queue.empty()) continue;
                line = queue.front();
                queue.pop_front();
            }

            try {
                if (sock && sock->isOpen()) {
                    sock->sendstr(line);
                    sent_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::lock_guard<std::mutex> lck(mtx);
                    queue.push_front(line);
                    reconnect = true;
                }
            } catch (const std::exception&) {
                std::lock_guard<std::mutex> lck(mtx);
                queue.push_front(line);
                reconnect = true;
            }
        }
    }

    std::string host = "127.0.0.1";
    int port = 10100;

    std::shared_ptr<net::Socket> sock;
    std::deque<std::string> queue;
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> running{false};
    std::atomic<uint64_t> sent_count{0};
    std::atomic<uint64_t> dropped_count{0};
    bool reconnect = false;
    std::thread worker;
    static constexpr size_t maxQueue = 1024;
};
