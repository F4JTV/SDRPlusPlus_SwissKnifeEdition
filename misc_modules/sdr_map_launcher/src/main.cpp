/*
 * SDR Map Launcher — SDR++ module
 *
 * Provides a small panel inside SDR++ to start / stop the companion Python
 * web server (the Django app sdr_map) and open its map in the default browser.
 *
 * The module does NOT touch the DSP path: no VFO, no audio. It is purely
 * "misc": four text fields, two buttons, a status indicator.
 *
 *   Web host / Web port  -> where the browser will reach the map page
 *   TCP host / TCP port  -> where decoder modules push their JSON frames
 *
 * Crucially, the child process is spawned ONLY when the user clicks "Start".
 * The module does not open any socket on its own — so toggling the plugin on
 * or off in SDR++ never holds a TCP socket, which avoids the "ghost module
 * connection" symptom seen with always-on decoder TCP outputs.
 */

#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <module.h>
#include <utils/flog.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <shellapi.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <arpa/inet.h>
    #include <errno.h>
    #include <fcntl.h>
    #include <netinet/in.h>
    #include <signal.h>
    #include <spawn.h>
    #include <sys/socket.h>
    #include <sys/select.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <unistd.h>
    #if defined(__linux__)
        #include <sys/prctl.h>
    #endif
    extern char** environ;
#endif

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "sdr_map_launcher",
    /* Description:     */ "Launches the SDR Map web server and opens it in the browser.",
    /* Author:          */ "F4JTV",
    /* Version:         */ 0, 1, 0,
    /* Max instances:   */ 1
};

ConfigManager config;

class SDRMapLauncherModule : public ModuleManager::Instance {
public:
    SDRMapLauncherModule(std::string name) {
        this->name = name;

        // Load persisted defaults (or fall back to sensible values).
        config.acquire();
        if (config.conf.contains("project_dir"))  projectDir  = config.conf["project_dir"].get<std::string>();
        if (config.conf.contains("python_bin"))   pythonBin   = config.conf["python_bin"].get<std::string>();
        if (config.conf.contains("web_host"))     webHost     = config.conf["web_host"].get<std::string>();
        if (config.conf.contains("web_port"))     webPort     = config.conf["web_port"].get<int>();
        if (config.conf.contains("tcp_host"))     tcpHost     = config.conf["tcp_host"].get<std::string>();
        if (config.conf.contains("tcp_port"))     tcpPort     = config.conf["tcp_port"].get<int>();
        config.release();

        copyToBuffers();

        // Last arg is the module instance pointer. Passing `NULL` tells
        // SDR++ this menu entry has no associated module to enable/disable,
        // so the core does not draw the on/off checkbox next to our name —
        // exactly like the satellite_tracker module. With `this` here, SDR++
        // would draw a checkbox driven by isEnabled() (which we don't need:
        // our Start/Stop buttons are the real on/off switch).
        gui::menu.registerEntry(name, menuHandler, this, NULL);
    }

    ~SDRMapLauncherModule() {
        stopServer();   // kill child if still running on module destruction
#if defined(_WIN32)
        if (childJob != nullptr) {
            CloseHandle(childJob);   // KILL_ON_JOB_CLOSE: kernel kills any
            childJob = nullptr;      // process still alive in the job
        }
#endif
        gui::menu.removeEntry(name);
    }

    void postInit() override {}

    // The "enabled" toggle in SDR++'s Module Manager is drawn by the core and
    // can't be hidden. We make it inert: enable/disable are no-ops and
    // isEnabled() always reports true so the module remains fully functional
    // whatever the user clicks. The Start/Stop buttons in our panel are the
    // real on/off switch.
    void enable()  override {}
    void disable() override {}
    bool isEnabled() override { return true; }

private:
    // ------------------------------------------------------------- GUI ---
    static void menuHandler(void* ctx) {
        SDRMapLauncherModule* _this = (SDRMapLauncherModule*)ctx;
        _this->drawMenu();
    }

    void drawMenu() {
        const float width = ImGui::GetContentRegionAvail().x;
        const bool serverRunning = isServerRunning();

        // While the server runs, the parameters are frozen (changing them
        // would have no effect on the running child).
        ImGui::BeginDisabled(serverRunning);

        ImGui::LeftLabel("Project dir");
        ImGui::SetNextItemWidth(width - ImGui::GetCursorPosX() + 8);
        if (ImGui::InputText(("##sml_proj_" + name).c_str(),
                             projectDirBuf, sizeof(projectDirBuf))) {
            projectDir = projectDirBuf;
            saveString("project_dir", projectDir);
        }

        ImGui::LeftLabel("Python");
        ImGui::SetNextItemWidth(width - ImGui::GetCursorPosX() + 8);
        if (ImGui::InputText(("##sml_py_" + name).c_str(),
                             pythonBinBuf, sizeof(pythonBinBuf))) {
            pythonBin = pythonBinBuf;
            saveString("python_bin", pythonBin);
        }

        ImGui::Spacing();

        ImGui::LeftLabel("Web host");
        ImGui::SetNextItemWidth(width - ImGui::GetCursorPosX() + 8);
        if (ImGui::InputText(("##sml_wh_" + name).c_str(),
                             webHostBuf, sizeof(webHostBuf))) {
            webHost = webHostBuf;
            saveString("web_host", webHost);
        }

        ImGui::LeftLabel("Web port");
        ImGui::SetNextItemWidth(width - ImGui::GetCursorPosX() + 8);
        if (ImGui::InputInt(("##sml_wp_" + name).c_str(), &webPort, 0)) {
            if (webPort < 1)     webPort = 1;
            if (webPort > 65535) webPort = 65535;
            saveInt("web_port", webPort);
        }

        ImGui::LeftLabel("TCP host");
        ImGui::SetNextItemWidth(width - ImGui::GetCursorPosX() + 8);
        if (ImGui::InputText(("##sml_th_" + name).c_str(),
                             tcpHostBuf, sizeof(tcpHostBuf))) {
            tcpHost = tcpHostBuf;
            saveString("tcp_host", tcpHost);
        }

        ImGui::LeftLabel("TCP port");
        ImGui::SetNextItemWidth(width - ImGui::GetCursorPosX() + 8);
        if (ImGui::InputInt(("##sml_tp_" + name).c_str(), &tcpPort, 0)) {
            if (tcpPort < 1)     tcpPort = 1;
            if (tcpPort > 65535) tcpPort = 65535;
            saveInt("tcp_port", tcpPort);
        }

        ImGui::EndDisabled();

        ImGui::Spacing();

        // ----- Buttons row -----
        const float btnW = (width - 8.0f) / 2.0f;
        if (!serverRunning) {
            if (ImGui::Button(("Start server##sml_start_" + name).c_str(),
                              ImVec2(btnW, 0))) {
                // Just start the server. The browser is no longer opened
                // automatically — the "Open map" button is right there for
                // when the operator wants to view the map.
                startServer();
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.20f, 0.20f, 1.0f));
            if (ImGui::Button(("Stop server##sml_stop_" + name).c_str(),
                              ImVec2(btnW, 0))) {
                stopServer();
            }
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
        if (ImGui::Button(("Open map##sml_open_" + name).c_str(),
                          ImVec2(btnW, 0))) {
            openBrowser();
        }

        // ----- Status line -----
        ImGui::Spacing();
        if (serverRunning) {
            ImGui::TextColored(ImVec4(0.20f, 0.85f, 0.40f, 1.0f),
                               "● Running (PID %lld)", (long long)childPid);
            ImGui::TextDisabled("http://%s:%d/",
                                displayHost(webHost).c_str(), webPort);
            ImGui::TextDisabled("Decoders → TCP %s:%d",
                                displayHost(tcpHost).c_str(), tcpPort);
        } else {
            ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f), "○ Stopped");
            if (!lastError.empty()) {
                ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f),
                                   "%s", lastError.c_str());
            }
        }
    }

    // ------------------------------------------- subprocess helpers ----
    bool isServerRunning() {
#if defined(_WIN32)
        if (childHandle == nullptr) return false;
        DWORD code = 0;
        if (GetExitCodeProcess(childHandle, &code) && code == STILL_ACTIVE) {
            return true;
        }
        // Process exited on its own: clean up.
        CloseHandle(childHandle);
        childHandle = nullptr;
        childPid = 0;
        return false;
#else
        if (childPid <= 0) return false;
        // 1) Try waitpid first (we are the parent).
        int status = 0;
        pid_t r = waitpid(childPid, &status, WNOHANG);
        if (r == childPid) {           // collected; really dead
            childPid = 0;
            return false;
        }
        if (r == 0) return true;       // alive and reapable
        // r == -1 (typically ECHILD: something else reaped it, or we're not
        // the parent anymore). DO NOT assume it's dead. Probe with kill(0).
        if (kill(childPid, 0) == 0) return true;     // still alive
        if (errno == ESRCH) {                         // really gone
            childPid = 0;
            return false;
        }
        // Any other errno (EPERM = exists but we lost permission): treat as
        // alive — better an orphan we can't kill than a false "stopped".
        return true;
#endif
    }

    void startServer() {
        lastError.clear();

        if (projectDir.empty()) {
            lastError = "Project dir is empty.";
            return;
        }
        if (pythonBin.empty()) {
            lastError = "Python binary path is empty.";
            return;
        }
        if (isServerRunning()) return;  // already up

#if defined(_WIN32)
        // Build the command line via cmd.exe so we can chain `migrate` then
        // `runserver_sdr`. The migrate step is idempotent and ensures the DB
        // is initialised on first run without requiring a separate terminal.
        std::string manage = projectDir + "\\manage.py";
        std::string cmdline =
            "cmd.exe /c "
            "\"\"" + pythonBin + "\" \"" + manage + "\" migrate --noinput && "
            "\"" + pythonBin + "\" \"" + manage + "\" runserver_sdr"
            + " --web-host " + webHost
            + " --web-port " + std::to_string(webPort)
            + " --tcp-host " + tcpHost
            + " --tcp-port " + std::to_string(tcpPort)
            + " --quiet\"";

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};

        std::vector<char> cmdBuf(cmdline.begin(), cmdline.end());
        cmdBuf.push_back('\0');

        // CREATE_SUSPENDED so we can assign the child to a Job Object BEFORE
        // it starts. The Job Object is configured with
        // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE so that when our handle to it
        // closes (module destruction OR SDR++ exit) the kernel kills every
        // process in the job. This is the Windows equivalent of Linux
        // PR_SET_PDEATHSIG and guarantees the Django server doesn't survive
        // SDR++.
        BOOL ok = CreateProcessA(
            nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED,
            nullptr, projectDir.c_str(), &si, &pi);
        if (!ok) {
            lastError = "CreateProcess failed (code " +
                        std::to_string(GetLastError()) + ")";
            flog::error("sdr_map_launcher: {}", lastError);
            return;
        }

        if (childJob == nullptr) {
            childJob = CreateJobObjectA(nullptr, nullptr);
            if (childJob != nullptr) {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
                jeli.BasicLimitInformation.LimitFlags =
                    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                SetInformationJobObject(childJob,
                    JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
            }
        }
        if (childJob != nullptr) {
            AssignProcessToJobObject(childJob, pi.hProcess);
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        childHandle = pi.hProcess;
        childPid    = pi.dwProcessId;
#else
        // POSIX: fork() + exec() so we can call setsid() AND
        // prctl(PR_SET_PDEATHSIG) in the child between the two.
        //
        //   - setsid(): the child becomes its own session/group leader, so
        //     kill(-pid, …) on Stop reaches the whole tree reliably.
        //   - prctl(PR_SET_PDEATHSIG, SIGTERM) (Linux only): the kernel will
        //     send SIGTERM to the child automatically when the *parent*
        //     (SDR++ itself) dies. This guarantees the Django server is
        //     shut down when SDR++ exits, even if the user forgets to click
        //     Stop or SDR++ crashes — addressing the surviving-server bug.
        std::string manage = projectDir + "/manage.py";
        std::string webPortStr = std::to_string(webPort);
        std::string tcpPortStr = std::to_string(tcpPort);

        std::string sh =
            "cd \"" + projectDir + "\" && "
            "\"" + pythonBin + "\" \"" + manage + "\" migrate --noinput && "
            "exec \"" + pythonBin + "\" \"" + manage + "\" runserver_sdr"
            + " --web-host " + webHost
            + " --web-port " + webPortStr
            + " --tcp-host " + tcpHost
            + " --tcp-port " + tcpPortStr
            + " --quiet";

        // Stash the parent PID so the child can detect a parent that died
        // before prctl was called (a small but real race window).
        const pid_t parentPid = getpid();

        pid_t pid = fork();
        if (pid < 0) {
            lastError = std::string("fork failed: ") + strerror(errno);
            flog::error("sdr_map_launcher: {}", lastError);
            return;
        }
        if (pid == 0) {
            // ----- child -----
            // 1) New session: PGID == SID == PID, signals are easy to scope.
            if (setsid() == -1) { _exit(126); }
#if defined(__linux__)
            // 2) Die with the parent. If SDR++ exits without calling
            //    stopServer(), the kernel signals us SIGTERM automatically.
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            // Tiny race: the parent may already be gone between fork and
            // prctl. In that case getppid() returns 1 (reparented to init).
            if (getppid() != parentPid) { _exit(0); }
#endif
            // 3) exec the shell wrapper. Use sh as the program so the build
            //    string can do "cd && migrate && exec runserver_sdr".
            char* const argv[] = {
                (char*)"/bin/sh", (char*)"-c", (char*)sh.c_str(), nullptr
            };
            execvp("/bin/sh", argv);
            // exec failed: best-effort exit so the parent sees us reaped.
            _exit(127);
        }

        // ----- parent -----
        childPid = pid;
#endif
        flog::info("sdr_map_launcher: server started (pid={}, "
                   "http://{}:{}/  TCP {}:{})", (int)childPid,
                   displayHost(webHost), webPort, displayHost(tcpHost), tcpPort);
    }

    // Probe whether something still answers on webHost:webPort. Used after
    // a stop to confirm the server is really gone — and to surface a clear
    // error if it isn't. Returns true if a TCP connection succeeds.
    bool isWebPortListening() {
#if defined(_WIN32)
        // Minimal Winsock probe (Winsock is already initialised by SDR++ core
        // through Asio/Boost). We just do connect() to be portable.
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons((u_short)webPort);
        const std::string h = displayHost(webHost);  // 0.0.0.0 -> 127.0.0.1
        inet_pton(AF_INET, h.c_str(), &addr.sin_addr);
        // Make the socket non-blocking with a short timeout via select().
        u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
        int r = connect(s, (sockaddr*)&addr, sizeof(addr));
        bool open = false;
        if (r == 0) {
            open = true;
        } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set wfd; FD_ZERO(&wfd); FD_SET(s, &wfd);
            timeval tv{0, 300 * 1000};   // 300 ms
            if (select(0, nullptr, &wfd, nullptr, &tv) > 0) open = true;
        }
        closesocket(s);
        return open;
#else
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(webPort);
        const std::string h = displayHost(webHost);
        inet_pton(AF_INET, h.c_str(), &addr.sin_addr);
        // Use non-blocking + select to avoid hanging on a stale port.
        int flags = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
        int r = connect(s, (sockaddr*)&addr, sizeof(addr));
        bool open = false;
        if (r == 0) {
            open = true;
        } else if (errno == EINPROGRESS) {
            fd_set wfd; FD_ZERO(&wfd); FD_SET(s, &wfd);
            timeval tv{0, 300 * 1000};   // 300 ms
            if (select(s + 1, nullptr, &wfd, nullptr, &tv) > 0) {
                int err = 0; socklen_t len = sizeof(err);
                getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err == 0) open = true;
            }
        }
        close(s);
        return open;
#endif
    }

    void stopServer() {
#if defined(_WIN32)
        if (childHandle == nullptr) return;
        // Quick attempt: CTRL_BREAK to the process group (graceful), with a
        // short grace period. We don't wait long — operators want the server
        // off NOW, not in 3 seconds.
        if (childPid != 0) {
            GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, (DWORD)childPid);
        }
        if (WaitForSingleObject(childHandle, 1000) != WAIT_OBJECT_0) {
            // Didn't exit in 1 s — kill it hard.
            TerminateProcess(childHandle, 1);
            WaitForSingleObject(childHandle, 2000);
        }
        CloseHandle(childHandle);
        childHandle = nullptr;
        childPid    = 0;
#else
        if (childPid <= 0) return;
        const pid_t pgid = childPid;

        // Step 1: polite SIGTERM. Send to BOTH the PID directly AND its
        // process group, so we hit the target no matter how the spawn went
        // (SETSID guarantees pgid == pid, but belt-and-braces).
        kill(pgid, SIGTERM);
        kill(-pgid, SIGTERM);

        // Wait up to 1 s for the child to exit.
        bool reaped = false;
        for (int i = 0; i < 10; i++) {
            int status = 0;
            pid_t r = waitpid(pgid, &status, WNOHANG);
            if (r == pgid) { reaped = true; break; }
            // Also accept "process is gone" via kill(0) — covers the case
            // where waitpid lost track of it for some reason.
            if (kill(pgid, 0) == -1 && errno == ESRCH) { reaped = true; break; }
            usleep(100 * 1000);
        }

        // Step 2: didn't exit -> SIGKILL, no negotiation. PID + group again.
        if (!reaped) {
            kill(pgid, SIGKILL);
            kill(-pgid, SIGKILL);
            // Try one more reap, but don't block forever.
            for (int i = 0; i < 10; i++) {
                if (kill(pgid, 0) == -1 && errno == ESRCH) break;
                usleep(100 * 1000);
            }
            waitpid(pgid, nullptr, WNOHANG);
        }

        // Step 3: reap any other zombie attributed to us in the group.
        while (waitpid(-pgid, nullptr, WNOHANG) > 0) { /* collected */ }
        childPid = 0;
#endif

        // Step 4: verify the web port is actually free. If something still
        // answers there, the kill didn't reach the right process (another
        // server is running on the same port? rogue python somewhere?). Tell
        // the user — silently pretending things are stopped would be worse.
        // We poll briefly because the kernel takes a fraction of a second
        // to release a listening socket after SIGKILL.
        bool stillListening = false;
        for (int i = 0; i < 8; i++) {     // up to ~1.2 s
            stillListening = isWebPortListening();
            if (!stillListening) break;
            usleep(150 * 1000);
        }
        if (stillListening) {
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                "Stop signalled, but %s:%d is still answering. "
                "Another process is using this port (or a stale server). "
                "Check 'ss -tlnp | grep :%d' or 'netstat -ano' on Windows.",
                displayHost(webHost).c_str(), webPort, webPort);
            lastError = msg;
            flog::error("sdr_map_launcher: {}", lastError);
        } else {
            flog::info("sdr_map_launcher: server stopped, port {} free", webPort);
        }
    }

    // -------------------------------------------------- browser launch ---
    void openBrowser() {
        const std::string host = displayHost(webHost);
        const std::string url  = "http://" + host + ":" + std::to_string(webPort) + "/";

#if defined(_WIN32)
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
        std::string cmd = "open \"" + url + "\" >/dev/null 2>&1 &";
        (void)system(cmd.c_str());
#else
        std::string cmd = "xdg-open \"" + url + "\" >/dev/null 2>&1 &";
        (void)system(cmd.c_str());
#endif
    }

    // 0.0.0.0 → 127.0.0.1 for display/link purposes (you can't connect to
    // the wildcard from a browser; show the loopback instead).
    static std::string displayHost(const std::string& h) {
        if (h == "0.0.0.0" || h.empty()) return "127.0.0.1";
        return h;
    }

    // ----------------------------------------------- config helpers ---
    void saveString(const char* key, const std::string& v) {
        config.acquire();
        config.conf[key] = v;
        config.release(true);
    }
    void saveInt(const char* key, int v) {
        config.acquire();
        config.conf[key] = v;
        config.release(true);
    }

    void copyToBuffers() {
        std::strncpy(projectDirBuf, projectDir.c_str(), sizeof(projectDirBuf)-1);
        std::strncpy(pythonBinBuf,  pythonBin.c_str(),  sizeof(pythonBinBuf)-1);
        std::strncpy(webHostBuf,    webHost.c_str(),    sizeof(webHostBuf)-1);
        std::strncpy(tcpHostBuf,    tcpHost.c_str(),    sizeof(tcpHostBuf)-1);
    }

    // ------------------------------------------------------- fields ---
    std::string name;

    // Configurable settings (persisted in JSON). The defaults point at the
    // location where install.sh / install.bat places the bundled Django
    // project, so a fresh install works out of the box without any path
    // tweaking — the user just clicks Start.
    std::string projectDir =
#if defined(_WIN32)
        std::string(std::getenv("LOCALAPPDATA") ? std::getenv("LOCALAPPDATA") : "C:\\")
        + "\\sdr_map_launcher\\sdr_map";
#else
        std::string(std::getenv("HOME") ? std::getenv("HOME") : "")
        + "/.local/share/sdr_map_launcher/sdr_map";
#endif
    std::string pythonBin =
#if defined(_WIN32)
        "python.exe";
#else
        "python3";
#endif
    std::string webHost  = "0.0.0.0";
    int         webPort  = 8000;
    std::string tcpHost  = "0.0.0.0";
    int         tcpPort  = 10100;

    // ImGui text-buffer copies (InputText writes into these).
    char projectDirBuf[512]{};
    char pythonBinBuf[256]{};
    char webHostBuf[64]{};
    char tcpHostBuf[64]{};

    // Child process handle / pid.
#if defined(_WIN32)
    HANDLE  childHandle = nullptr;
    HANDLE  childJob    = nullptr;   // Job Object: KILL_ON_JOB_CLOSE
    DWORD   childPid    = 0;
#else
    pid_t   childPid    = 0;
#endif
    std::string lastError;
};

// ----------------------------------------- module manager wiring ----------
MOD_EXPORT void _INIT_() {
    json def = json({});
    def["project_dir"] = "";
    def["python_bin"]  = "";
    def["web_host"]    = "0.0.0.0";
    def["web_port"]    = 8000;
    def["tcp_host"]    = "0.0.0.0";
    def["tcp_port"]    = 10100;
    config.setPath(core::args["root"].s() + "/sdr_map_launcher_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new SDRMapLauncherModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (SDRMapLauncherModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
