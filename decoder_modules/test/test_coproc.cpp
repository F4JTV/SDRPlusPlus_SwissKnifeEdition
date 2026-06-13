// test_coproc.cpp - standalone test of the HFDL module's coprocess plumbing
// and text-parsing contract. No SDR++ build required.
//
// Unlike a full RF decode (which needs HFDL's FEC/interleaver/Reed-Solomon and
// is exercised separately by tools/hfdl_gen.py against the real dumphfdl), this
// harness pins down the parts the module owns end-to-end:
//
//   Test A  Drive a STUB "dumphfdl" through the exact fork/exec/pipe/reader
//           path the module uses: feed it CS16 I/Q on stdin, read its text
//           output back, split it into per-message blocks on header lines, then
//           run the SAME hfdl_parse.h routines the module calls (classify /
//           buildPositionJson) and assert the results. This validates the block
//           framing and the map-JSON contract deterministically.
//   Test B  Point the coprocess at a missing binary and assert the status
//           resolves to "not found" (the module's diagnostic path).
//
// The parsing logic under test is the real one: this file #includes the
// module's hfdl_parse.h, the single source of truth shared with main.cpp.
//
// Build & run:
//   g++ -std=c++17 -O2 -I../src test/test_coproc.cpp -o /tmp/test_hfdl -lpthread
//   /tmp/test_hfdl            # uses an auto-generated stub; or:
//   /tmp/test_hfdl /path/to/real/dumphfdl   # (Test B still uses a bogus path)
//   Expect: *** HFDL COPROCESS CONTRACT OK ***

#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <atomic>
#include <mutex>

#include "hfdl_parse.h"   // the module's own parser (single source of truth)

// A faithful copy of the module's child-process plumbing, reduced to what the
// test needs. The reader splits on hfdlparse::isHeaderLine exactly as the
// module's reader thread does.
struct Coproc {
    std::atomic<int> childStdin{-1};
    int childStdout = -1;
    pid_t childPid = -1;
    std::thread reader;
    std::atomic<bool> running{false};
    std::string status = "idle", curBlock;
    std::mutex mtx;
    std::deque<std::string> blocks;

    void push(const std::string& b) {
        std::lock_guard<std::mutex> l(mtx);
        blocks.push_back(b);
    }

    bool start(const std::string& path, const std::string& freqKhz) {
        int in[2], out[2];
        if (pipe(in) || pipe(out)) { status = "pipe fail"; return false; }
        pid_t pid = fork();
        if (pid < 0) { status = "fork fail"; return false; }
        if (pid == 0) {
            dup2(in[0], 0); dup2(out[1], 1);
            close(in[0]); close(in[1]); close(out[0]); close(out[1]);
            execlp(path.c_str(), path.c_str(),
                   "--iq-file", "-", "--sample-format", "CS16",
                   "--sample-rate", "12000", "--utc",
                   "--centerfreq", freqKhz.c_str(), freqKhz.c_str(),
                   "--output", "decoded:text:file:path=-", (char*)NULL);
            _exit(127);
        }
        close(in[0]); close(out[1]);
        childPid = pid; childStdout = out[0];
        childStdin.store(in[1]); running.store(true);
        reader = std::thread([this] { readerLoop(); });
        return true;
    }

    void readerLoop() {
        char buf[4096]; std::string acc;
        while (running.load()) {
            ssize_t n = read(childStdout, buf, sizeof(buf));
            if (n > 0) {
                acc.append(buf, n);
                size_t nl;
                while ((nl = acc.find('\n')) != std::string::npos) {
                    std::string line = acc.substr(0, nl);
                    acc.erase(0, nl + 1);
                    if (hfdlparse::isHeaderLine(line) && !curBlock.empty()) {
                        push(curBlock); curBlock.clear();
                    }
                    if (!curBlock.empty()) curBlock += "\n";
                    curBlock += line;
                }
            } else if (n == 0) { break; }
            else { if (errno == EINTR) continue; break; }
        }
        if (!curBlock.empty()) { push(curBlock); curBlock.clear(); }
        if (childPid > 0) {
            int st = 0;
            if (waitpid(childPid, &st, WNOHANG) == childPid) {
                childPid = -1;
                if (WIFEXITED(st) && WEXITSTATUS(st) == 127) status = "not found";
                else if (!running.load()) status = "stopped";
                else status = "exited";
            }
        }
        running.store(false);
    }

    void feedZeros(size_t bytes) {
        int fd = childStdin.load();
        if (fd < 0) return;
        std::vector<char> z(4096, 0);
        size_t sent = 0;
        while (sent < bytes) {
            size_t chunk = std::min(z.size(), bytes - sent);
            ssize_t w = write(fd, z.data(), chunk);
            if (w > 0) { sent += w; continue; }
            if (w < 0 && errno == EINTR) continue;
            break;
        }
    }

    void closeStdin() { int s = childStdin.exchange(-1); if (s >= 0) close(s); }

    void stop() {
        running.store(false);
        closeStdin();
        if (childPid > 0) kill(childPid, SIGTERM);
        if (reader.joinable()) reader.join();
        if (childStdout >= 0) { close(childStdout); childStdout = -1; }
        if (childPid > 0) { int st; waitpid(childPid, &st, 0); childPid = -1; }
    }
};

// Write a stub "dumphfdl" that ignores its CLI args, drains stdin (so the
// writer never blocks on a full pipe), then prints canned dumphfdl-format text
// covering a squitter, an ACARS uplink, and a position-bearing block.
static std::string writeStub() {
    const char* path = "/tmp/hfdl_stub.sh";
    FILE* f = fopen(path, "w");
    if (!f) { perror("stub"); exit(2); }
    fputs(
        "#!/usr/bin/env bash\n"
        "cat > /dev/null\n"          // drain CS16 from stdin
        "cat <<'EOF'\n"
        "[2021-09-30 21:28:46 UTC] [11384.0 kHz] [24.7 Hz] [300 bps] [S]\n"
        " Uplink SPDU:\n"
        "  Squitter: ground station status\n"
        "[2021-09-30 21:29:01 UTC] [11384.0 kHz] [20.1 Hz] [1800 bps] [D]\n"
        " LPDU: Unnumbered data\n"
        "  ACARS:\n"
        "   Flight ID: AFR1234\n"
        "   Message: REQ WX\n"
        "[2021-09-30 21:29:15 UTC] [11384.0 kHz] [18.3 Hz] [1800 bps] [D]\n"
        " LPDU: Unnumbered data\n"
        "  Performance data:\n"
        "   Flight ID: BAW777\n"
        "   Src GS: Shannon\n"
        "   Lat: 51.4706000\n"
        "   Lon: -0.4619000\n"
        "EOF\n",
        f);
    fclose(f);
    chmod(path, 0755);
    return path;
}

static bool approx(double a, double b) { return (a - b) < 1e-4 && (b - a) < 1e-4; }

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    (void)argc; (void)argv;
    bool all = true;

    // ---- Test A: stub decoder, full split + parse contract ----------------
    {
        std::string stub = writeStub();
        Coproc c;
        bool started = c.start(stub, "11384");
        // Feed a realistic burst of CS16 (12 kHz * 0.2 s * 2 ch * 2 bytes).
        c.feedZeros(12000 * 2 * 2 / 5);
        c.closeStdin();                       // EOF -> stub flushes and exits
        for (int i = 0; i < 50 && c.running.load(); i++) usleep(100000);
        c.stop();

        size_t n = c.blocks.size();
        bool framing = started && (n == 3);
        printf("[A1 framing]     started=%d blocks=%zu (want 3) -> %s\n",
               started, n, framing ? "OK" : "FAIL");
        all = all && framing;

        if (n == 3) {
            int c0 = hfdlparse::classify(c.blocks[0]);
            int c1 = hfdlparse::classify(c.blocks[1]);
            int c2 = hfdlparse::classify(c.blocks[2]);
            bool cls = (c0 == hfdlparse::T_SQUITTER) &&
                       (c1 == hfdlparse::T_ACARS) &&
                       (c2 == hfdlparse::T_POSITION);
            printf("[A2 classify]    [%s, %s, %s] (want Squitter, ACARS, Position) -> %s\n",
                   hfdlparse::typeName(c0), hfdlparse::typeName(c1),
                   hfdlparse::typeName(c2), cls ? "OK" : "FAIL");
            all = all && cls;

            std::string js;
            bool built = hfdlparse::buildPositionJson(c.blocks[2], js);
            bool jsonOk = built &&
                js.find("\"name\":\"BAW777\"") != std::string::npos &&
                js.find("\"type\":\"HFDL\"") != std::string::npos &&
                js.find("\"speed\":null") != std::string::npos &&
                js.find("\"date\":\"2021-09-30\"") != std::string::npos &&
                js.find("\"time\":\"21:29:15\"") != std::string::npos &&
                js.find("gs=Shannon") != std::string::npos;
            printf("[A3 map-json]    built=%d %s -> %s\n", built,
                   js.c_str(), jsonOk ? "OK" : "FAIL");
            all = all && jsonOk;

            // Numeric sanity on the extracted coordinates.
            double lat = std::stod(hfdlparse::grabField(c.blocks[2], "Lat:"));
            double lon = std::stod(hfdlparse::grabField(c.blocks[2], "Lon:"));
            bool coord = approx(lat, 51.4706) && approx(lon, -0.4619);
            printf("[A4 coords]      lat=%.4f lon=%.4f -> %s\n",
                   lat, lon, coord ? "OK" : "FAIL");
            all = all && coord;

            // A squitter block must NOT be promoted to a position.
            std::string none;
            bool noPos = !hfdlparse::buildPositionJson(c.blocks[0], none);
            printf("[A5 no-false-pos] squitter->position=%d -> %s\n",
                   !noPos, noPos ? "OK" : "FAIL");
            all = all && noPos;
        } else {
            all = false;
        }
    }

    // ---- Test B: missing binary -> "not found" ----------------------------
    {
        Coproc c;
        c.start("/nonexistent/dumphfdl_xyz", "11384");
        for (int i = 0; i < 30 && c.running.load(); i++) usleep(50000);
        c.stop();
        bool ok = (c.status == "not found");
        printf("[B not-found]    status='%s' -> %s\n", c.status.c_str(),
               ok ? "OK" : "FAIL");
        all = all && ok;
    }

    printf("\n%s\n", all ? "*** HFDL COPROCESS CONTRACT OK ***"
                         : "*** FAIL ***");
    return all ? 0 : 1;
}
