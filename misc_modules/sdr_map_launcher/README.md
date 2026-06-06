# SDR Map Launcher — SDR++ module

A small SDR++ panel to start / stop the companion `sdr_map` Django web server
and open the live map in the default browser, without leaving SDR++.

This package bundles **everything** in one tarball:

- the SDR++ module sources (`src/main.cpp`, `CMakeLists.txt`),
- the full Django project (`sdr_map/`) with all migrations and static assets,
- an installer (`install.sh` / `install.bat`) that handles deployment.

## What it does

The module adds a menu entry in SDR++ with:

| Field        | What it sets                                                  |
|--------------|---------------------------------------------------------------|
| Project dir  | Path to the `sdr_map/` folder (the one with `manage.py`)      |
| Python       | Path to the Python interpreter (`python3`, `python.exe`, ...) |
| Web host     | Address the web server binds to (`0.0.0.0` for all interfaces)|
| Web port     | Web server port (default `8000`)                              |
| TCP host     | Address the TCP collector binds to (default `0.0.0.0`)        |
| TCP port     | Single TCP port for decoder modules (default `10100`)         |
| Open browser after start | If ticked, opens the map automatically            |

Two buttons:

- **Start server / Stop server** spawns / stops the Python child running
  `python manage.py runserver_sdr ... --quiet`.
- **Open map** opens `http://<web_host>:<web_port>/` in the default browser
  (any time, whether the server is running or not).

While the server runs, the parameter fields are disabled (changing them
would have no effect on the running child). Stop the server to edit, then
start again.

## Quick install (Linux / macOS)

```bash
tar xzf sdr_map_launcher.tar.gz
cd sdr_map_launcher
./install.sh /path/to/SDRPlusPlus      # builds & installs the .so too
```

What it does:

1. Copies the Django project to `~/.local/share/sdr_map_launcher/sdr_map`
   (backs up any existing install to `.../sdr_map.bak` and carries
   `db.sqlite3` over so you don't lose history or settings).
2. Installs Python dependencies (Django, channels, daphne, whitenoise) for
   the system `python3`. Tries regular `pip`, then falls back to
   `pip install --user --break-system-packages` to handle PEP 668 on
   Ubuntu 24 / Debian 12+.
3. If you pass an SDR++ source path: drops the module into
   `misc_modules/sdr_map_launcher/`, registers it in the root CMakeLists,
   builds it, and copies `sdr_map_launcher.so` into `/usr/lib/sdrpp/plugins/`.

Skip the SDR++ path to only do steps 1 and 2 (useful to update the Django
project without rebuilding the module):

```bash
./install.sh                           # only steps 1 and 2
```

## Quick install (Windows)

```bat
tar -xzf sdr_map_launcher.tar.gz       :: Windows 10+ has tar built-in
cd sdr_map_launcher
install.bat                            :: copies project + installs Python deps
```

For the SDR++ module itself, follow the on-screen build instructions or use
`apply_to_sdrpp.sh` under Git Bash / WSL — the build step still relies on
CMake + MSVC (or MinGW), as for any other SDR++ module.

## Default Project dir matches the installer

The module's "Project dir" field is pre-filled with:

- Linux / macOS: `~/.local/share/sdr_map_launcher/sdr_map`
- Windows: `%LOCALAPPDATA%\sdr_map_launcher\sdr_map`

That's exactly where the installer puts the Django project, so on first
launch you literally just click **Start server**. If you keep the Django
project elsewhere, just point the field at it.

## Works without a virtualenv

The launcher invokes the Python interpreter you configure in the **Python**
field. No virtualenv required: as long as
`python3 -c "import django"` works in your terminal, the launcher can start
the server. The installer does that for you on Linux/macOS.

If you do use a venv, point the **Python** field at the venv's interpreter
(`/path/to/venv/bin/python3`) — works identically.

## Automatic `migrate` on every start

Before launching the server, the module runs
`python manage.py migrate --noinput` so that:

- on first use, the SQLite database is created and all tables installed
  without any prior terminal step;
- on subsequent starts, it is a no-op (`No migrations to apply`).

A fresh install just works: click **Start server** and the map is ready.

## Why this is safe (no ghost TCP connection)

The module **does not open any network socket on its own**. The TCP collector
is created by the Python child only when you click **Start**. Enabling or
disabling the plugin in SDR++'s Module Manager has zero effect on TCP. This
deliberately sidesteps the issue where a decoder module's "TCP output" stays
connected to a server even when the module is disabled — the launcher is the
one process that decides whether the server runs.

## Manual build (Linux / macOS)

If you'd rather not use `install.sh`:

```bash
git clone --depth 1 https://github.com/AlexandreRouma/SDRPlusPlus.git
cd sdr_map_launcher
./apply_to_sdrpp.sh /path/to/SDRPlusPlus

cd /path/to/SDRPlusPlus
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DOPT_BUILD_SDR_MAP_LAUNCHER=ON
make -j$(nproc)
sudo make install
```

Then copy `sdr_map/` somewhere (e.g. `~/.local/share/sdr_map_launcher/sdr_map`)
and point the module's "Project dir" field at it.

## Enable in SDR++

1. Launch SDR++.
2. Open the **Module Manager** (left sidebar).
3. In the dropdown at the bottom, pick `sdr_map_launcher`.
4. Type an instance name (e.g. `MAP`) and click **+**.
5. The "MAP" entry appears in the side menu — click it to open the panel.

## Persistence

All fields are saved automatically to `sdr_map_launcher_config.json` (next
to SDR++'s other config files) so the next start of SDR++ remembers your
project dir, Python path, hosts and ports.

## Notes

- On Stop, the launcher sends SIGTERM to the whole Python process group;
  Daphne reacts to it and shuts the web server + TCP collector cleanly. If
  the child is still alive after 3 seconds, SIGKILL is sent as a fallback.
- The browser is opened with `xdg-open` (Linux), `open` (macOS), or
  `ShellExecute` (Windows).
- `0.0.0.0` is shown as `127.0.0.1` in the displayed URL (you can't browse
  to the wildcard address; this is just for display).
- The status line shows the live URL, the TCP collector address, and the
  PID of the running child.

## License

BSD 3-Clause.
