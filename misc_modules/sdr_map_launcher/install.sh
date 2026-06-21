#!/usr/bin/env bash
#
# install.sh — one-shot installer for the SDR Map Launcher module.
#
# Does three things:
#   1) Copy the bundled Django project to ~/.local/share/sdr_map_launcher/sdr_map
#   2) Install the Python dependencies (Django, channels, daphne, whitenoise)
#      at the system / user level (no virtualenv used)
#   3) Drop the module into an SDR++ source tree, build it and install the .so
#
# Usage:
#   ./install.sh [/path/to/SDRPlusPlus]
#
# If SDR++ source path is omitted, only steps 1) and 2) run — useful when you
# only want to update the Django project and have already built the module.
#
set -e

HERE="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
SDRPP_DIR="${1:-}"
DATA_DIR="${HOME}/.local/share/sdr_map_launcher"
PROJECT_DST="${DATA_DIR}/sdr_map"

echo "==> SDR Map Launcher — installer"
echo "    Bundle path : $HERE"
echo "    Data dir    : $DATA_DIR"

# ---------------------------------------------------------------- 1) Django
echo ""
echo "[1/3] Installing the Django project..."
mkdir -p "$DATA_DIR"
if [ -d "$PROJECT_DST" ]; then
    echo "    Existing project found — backing it up to ${PROJECT_DST}.bak"
    rm -rf "${PROJECT_DST}.bak"
    mv "$PROJECT_DST" "${PROJECT_DST}.bak"
fi
cp -r "$HERE/sdr_map" "$PROJECT_DST"
echo "    ✓ Django project copied to $PROJECT_DST"

# Carry over the existing DB if the user had one (most useful field/data)
if [ -f "${PROJECT_DST}.bak/db.sqlite3" ]; then
    cp "${PROJECT_DST}.bak/db.sqlite3" "$PROJECT_DST/db.sqlite3"
    echo "    ✓ Previous db.sqlite3 carried over"
fi

# ---------------------------------------------------- 2) Python deps
echo ""
echo "[2/3] Installing Python dependencies for the system 'python3'..."
PY="${PYTHON:-python3}"
if ! command -v "$PY" >/dev/null 2>&1; then
    echo "    ! '$PY' not found. Install Python 3 first."
    exit 1
fi

if "$PY" -c "import django, channels, daphne, whitenoise" 2>/dev/null; then
    echo "    ✓ Dependencies already present (django, channels, daphne, whitenoise)"
else
    REQ="$PROJECT_DST/requirements.txt"
    # PEP 668 (Ubuntu 24, Debian 12+): system pip blocks installs by default.
    # We try once normally; if it fails, retry with --break-system-packages.
    if "$PY" -m pip install -r "$REQ" 2>/dev/null; then
        echo "    ✓ Installed (regular pip)"
    elif "$PY" -m pip install --user --break-system-packages -r "$REQ"; then
        echo "    ✓ Installed (--user --break-system-packages)"
    else
        echo "    ! Could not install dependencies automatically. Install manually:"
        echo "      $PY -m pip install --user --break-system-packages -r $REQ"
        exit 1
    fi
fi

# ------------------------------------ 2bis) Aircraft enrichment database
#
# Downloads the Mictronics ICAO 24-bit -> (registration, type code, military)
# dataset and converts it to a local SQLite indexed by ICAO. This is the
# Phase B enrichment that gives ADS-B markers their precise model, the
# operator registration, the military badge, and helps disambiguate icons
# (a Eurofighter transmits only "no info" as wake-vortex category but is
# correctly identified as a fighter via its type code EUFI here).
#
# Non-fatal: if the download fails (no internet, GitHub down) the rest of
# the install continues and the server runs in Phase A mode (wake-vortex
# category only). To retry later: run tools/build_aircraft_db.py manually.
echo ""
echo "[2bis] Downloading aircraft enrichment database (Mictronics, ~14 MB)..."
ACFT_DB="${DATA_DIR}/aircraft_db.sqlite"
if "$PY" "$HERE/tools/build_aircraft_db.py" "$ACFT_DB"; then
    echo "    ✓ Aircraft DB ready at $ACFT_DB"
else
    echo "    ! Aircraft DB download failed — server will run in Phase A mode"
    echo "      (wake-vortex category icons still work; just no model lookup)"
    echo "      Retry later with: $PY $HERE/tools/build_aircraft_db.py"
fi

# ---------------------------------------------------- 3) SDR++ module
if [ -z "$SDRPP_DIR" ]; then
    echo ""
    echo "[3/3] Skipped (no SDR++ source path given)."
    echo ""
    echo "To install the module later:"
    echo "  $0 /path/to/SDRPlusPlus"
    echo ""
    echo "You can already test the Django side directly:"
    echo "  cd $PROJECT_DST && $PY manage.py migrate && $PY manage.py runserver_sdr"
    echo ""
    echo "Done."
    exit 0
fi

if [ ! -f "$SDRPP_DIR/CMakeLists.txt" ]; then
    echo "    ! '$SDRPP_DIR' does not look like the SDR++ source tree."
    exit 1
fi

echo ""
echo "[3/3] Building the SDR++ module in-tree..."
"$HERE/apply_to_sdrpp.sh" "$SDRPP_DIR"

BUILD_DIR="$SDRPP_DIR/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release -DOPT_BUILD_SDR_MAP_LAUNCHER=ON >/dev/null
make -j"$(nproc 2>/dev/null || echo 2)" sdr_map_launcher

SO="$BUILD_DIR/misc_modules/sdr_map_launcher/sdr_map_launcher.so"
if [ ! -f "$SO" ]; then
    echo "    ! Build failed: $SO not found."
    exit 1
fi

# Place the .so in the SDR++ plugins directory (system or per-user).
SYS_PLUGINS=""
for cand in /usr/lib/sdrpp/plugins /usr/local/lib/sdrpp/plugins; do
    if [ -d "$cand" ]; then SYS_PLUGINS="$cand"; break; fi
done

if [ -n "$SYS_PLUGINS" ] && [ -w "$SYS_PLUGINS" ]; then
    cp "$SO" "$SYS_PLUGINS/"
    echo "    ✓ Installed to $SYS_PLUGINS/sdr_map_launcher.so"
elif [ -n "$SYS_PLUGINS" ]; then
    echo "    ! $SYS_PLUGINS is not writable. Run:"
    echo "      sudo cp $SO $SYS_PLUGINS/"
else
    echo "    · No system plugins dir found. Build artefact:"
    echo "      $SO"
fi

echo ""
echo "Done."
echo "  Project Django: $PROJECT_DST"
echo "  Module .so    : $SO"
echo ""
echo "In SDR++: Module Manager -> pick 'sdr_map_launcher' -> add an instance,"
echo "then click 'Start server'. The default Project dir is already correct."
