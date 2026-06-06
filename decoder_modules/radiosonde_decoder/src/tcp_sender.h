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
// Django map collector. Same design as the AIS / ADS-B / APRS / DSD-FME
// modules:
//   * a dedicated worker thread owns the socket so the DSP / decoder threads
//     are never blocked by network I/O;
//   * lines are queued and flushed by the worker;
//   * the connection is (re)established lazily and retried automatically with
//     a backoff, so the map server can come and go without disturbing the
//     decoder.
class TcpLineSender {
public:
    TcpLineSender() {}
    ~TcpLineSender() { stop(); }

    // Reconfigure target. Safe to call while running; the worker drops the
    // current socket and reconnects to the new host/port on its next cycle.
    void configure(const std::string& host, int port) {
        std::lock_guard<std::mutex> lck(mtx);
        if (host != this->host || port != this->port) {
            this->host = host;
            this->port = port;
            reconnect = true;
        }
    }

    void start() {
        if (running) return;
        running = true;
        worker = std::thread(&TcpLineSender::run, this);
    }

    void stop() {
        if (!running) return;
        running = false;
        cv.notify_all();
        if (worker.joinable()) worker.join();
        if (sock && sock->isOpen()) sock->close();
        sock.reset();
    }

    /* True between start() and stop() (the worker thread is alive). */
    bool isRunning() const { return running.load(); }

    /* True when the worker currently holds an open socket to the server.
     * Advisory (may change concurrently). */
    bool isConnected() {
        std::lock_guard<std::mutex> lck(mtx);
        return sock && sock->isOpen();
    }

    // Queue one line (JSON + '\n' is added here). Never blocks the caller.
    void send(const std::string& line) {
        {
            std::lock_guard<std::mutex> lck(mtx);
            // Bound the queue so a long network outage can't grow memory
            // without limit; drop oldest if necessary.
            if (queue.size() >= maxQueue) queue.pop_front();
            queue.push_back(line + "\n");
        }
        cv.notify_one();
    }

private:
    void run() {
        while (running) {
            // (Re)connect if needed.
            bool needConn;
            std::string h; int p;
            {
                std::lock_guard<std::mutex> lck(mtx);
                needConn = reconnect || !sock || !sock->isOpen();
                h = host; p = port;
                reconnect = false;
            }
            if (needConn) {
                if (sock && sock->isOpen()) sock->close();
                sock.reset();
                try {
                    sock = net::connect(h, p);
                    flog::info("[radiosonde] TCP map link connected to {}:{}", h, p);
                } catch (const std::exception& e) {
                    // Server not up yet; wait and retry without spamming.
                    std::unique_lock<std::mutex> lck(mtx);
                    cv.wait_for(lck, std::chrono::seconds(5),
                                [&]{ return !running || reconnect; });
                    continue;
                }
            }

            // Wait for something to send.
            std::string line;
            {
                std::unique_lock<std::mutex> lck(mtx);
                cv.wait(lck, [&]{ return !running || reconnect || !queue.empty(); });
                if (!running) break;
                if (reconnect) continue;
                if (queue.empty()) continue;
                line = queue.front();
                queue.pop_front();
            }

            // Send; on failure, requeue the line and force a reconnect.
            try {
                if (sock && sock->isOpen()) {
                    sock->sendstr(line);
                } else {
                    std::lock_guard<std::mutex> lck(mtx);
                    queue.push_front(line);
                    reconnect = true;
                }
            } catch (const std::exception& e) {
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
    bool reconnect = false;
    std::thread worker;
    static constexpr size_t maxQueue = 1024;
};
