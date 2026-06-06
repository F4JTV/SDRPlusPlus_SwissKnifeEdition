#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <functional>
#include <cstdint>
#include <cstring>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <cerrno>
#endif

// Spawns dsd-fme as a child process:
//   - we WRITE 16-bit signed mono PCM (48 kHz) into its stdin   (dsd-fme -i -)
//   - we READ its merged stdout+stderr, split into lines, and hand each line
//     to a user callback (the textual decode events / payload log).
//
// Only the Linux/POSIX path is implemented (the module target is Ubuntu 24).
// On other platforms start() fails gracefully with an explanatory message so
// the rest of the module still compiles.
class DsdProcess {
public:
    using LineCallback = std::function<void(const std::string&)>;

    DsdProcess() {}
    ~DsdProcess() { stop(); }

    DsdProcess(const DsdProcess&) = delete;
    DsdProcess& operator=(const DsdProcess&) = delete;

    bool start(const std::vector<std::string>& args, LineCallback onLine) {
#ifdef _WIN32
        (void)args; (void)onLine;
        lastError = "dsd-fme subprocess spawning is only implemented for Linux in this build.";
        return false;
#else
        if (running) { return true; }
        onLineCb = std::move(onLine);

        int inPipe[2];   // parent -> child stdin
        int outPipe[2];  // child stdout/stderr -> parent
        if (pipe(inPipe) != 0)  { lastError = "pipe(stdin) failed"; return false; }
        if (pipe(outPipe) != 0) { ::close(inPipe[0]); ::close(inPipe[1]); lastError = "pipe(stdout) failed"; return false; }

        // Build the argv array BEFORE forking (no allocation in the child).
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args) { argv.push_back(const_cast<char*>(a.c_str())); }
        argv.push_back(nullptr);

        pid_t pid = fork();
        if (pid < 0) {
            ::close(inPipe[0]); ::close(inPipe[1]);
            ::close(outPipe[0]); ::close(outPipe[1]);
            lastError = "fork() failed";
            return false;
        }

        if (pid == 0) {
            // -------- child --------
            ::dup2(inPipe[0],  STDIN_FILENO);
            ::dup2(outPipe[1], STDOUT_FILENO);
            ::dup2(outPipe[1], STDERR_FILENO);
            ::close(inPipe[0]);  ::close(inPipe[1]);
            ::close(outPipe[0]); ::close(outPipe[1]);

            // Make our process group the leader so SIGTERM doesn't propagate
            // back up to SDR++ in odd shell setups.
            setpgid(0, 0);

            execvp(argv[0], argv.data());

            // exec failed (binary not found etc.)
            const char* msg = "[dsd_decoder] FATAL: failed to exec dsd-fme. Is it installed and on PATH?\n";
            ssize_t r = ::write(STDERR_FILENO, msg, std::strlen(msg)); (void)r;
            _exit(127);
        }

        // -------- parent --------
        childPid = pid;
        ::close(inPipe[0]);   // not reading child's stdin
        ::close(outPipe[1]);  // not writing child's stdout
        writeFd = inPipe[1];
        readFd  = outPipe[0];

        // Enlarge the stdin pipe so a brief decode stall never blocks us.
        fcntl(writeFd, F_SETPIPE_SZ, 1 << 20);

        running = true;
        eofSeen = false;
        readerThread = std::thread(&DsdProcess::readerLoop, this);
        return true;
#endif
    }

    // Blocking write of mono PCM samples to dsd-fme's stdin.
    // Call from a dedicated writer thread, never from the DSP thread.
    // Returns false once the child's stdin is broken (child died).
    bool writeSamples(const int16_t* data, size_t n) {
#ifdef _WIN32
        (void)data; (void)n; return false;
#else
        if (writeFd < 0) { return false; }
        const char* p = reinterpret_cast<const char*>(data);
        size_t total = n * sizeof(int16_t);
        size_t off = 0;
        while (off < total) {
            ssize_t w = ::write(writeFd, p + off, total - off);
            if (w > 0) { off += static_cast<size_t>(w); continue; }
            if (w < 0 && errno == EINTR) { continue; }
            return false; // EPIPE or other fatal -> child gone
        }
        return true;
#endif
    }

    void stop() {
#ifndef _WIN32
        running = false;
        if (writeFd >= 0) { ::close(writeFd); writeFd = -1; } // EOF on child stdin

        // Ensure the child is actually dead BEFORE joining the reader thread.
        // If the child hangs and we joined the reader first, the reader's
        // read() on the child's stdout would block forever and we'd deadlock
        // because the SIGKILL escalation below would never run.
        if (childPid > 0) {
            ::kill(childPid, SIGTERM);
            int status = 0;
            bool reaped = false;
            for (int i = 0; i < 50; i++) { // up to ~500 ms for a clean exit
                pid_t r = waitpid(childPid, &status, WNOHANG);
                if (r == childPid) { reaped = true; break; }
                if (r < 0)         { reaped = true; break; }
                usleep(10000);
            }
            if (!reaped) {
                ::kill(childPid, SIGKILL);
                waitpid(childPid, &status, 0);
            }
            childPid = -1;
        }

        // The child is dead, its stdout is closed -> our read() returns 0 -> reader exits.
        if (readerThread.joinable()) { readerThread.join(); }
        if (readFd >= 0) { ::close(readFd); readFd = -1; }
#endif
    }

    bool isRunning()        const { return running && !eofSeen; }
    bool childExited()      const { return eofSeen; }
    std::string lastErr()   const { return lastError; }

private:
#ifndef _WIN32
    void readerLoop() {
        char buf[8192];
        std::string acc;
        while (running) {
            ssize_t r = ::read(readFd, buf, sizeof(buf));
            if (r > 0) {
                acc.append(buf, static_cast<size_t>(r));
                size_t pos;
                while ((pos = acc.find('\n')) != std::string::npos) {
                    std::string line = acc.substr(0, pos);
                    acc.erase(0, pos + 1);
                    if (!line.empty() && line.back() == '\r') { line.pop_back(); }
                    if (onLineCb) { onLineCb(line); }
                }
                // Guard against a pathological no-newline flood.
                if (acc.size() > (1u << 20)) { acc.clear(); }
            }
            else if (r == 0) { eofSeen = true; break; } // child closed output
            else {
                if (errno == EINTR) { continue; }
                break;
            }
        }
        if (onLineCb && !acc.empty()) { onLineCb(acc); }
    }
#endif

    LineCallback onLineCb;
    std::thread readerThread;
    std::atomic<bool> running{false};
    std::atomic<bool> eofSeen{false};
    std::string lastError;
#ifndef _WIN32
    pid_t childPid = -1;
    int writeFd = -1;
    int readFd  = -1;
#endif
};
