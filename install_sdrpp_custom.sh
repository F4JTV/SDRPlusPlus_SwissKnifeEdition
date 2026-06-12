#!/usr/bin/bash
#
# Compilation / installation de SDRPlusPlus + modules custom (F4JTV)

set -euo pipefail

# usage: clone_or_update <url> <dossier_cible> [branche]
clone_or_update() {
    url="$1"; dst="$2"; branch="${3:-}"
    if [ -d "$dst/.git" ]; then
        echo ">> maj $dst"
        git -C "$dst" fetch --all --prune
        if [ -n "$branch" ]; then git -C "$dst" checkout "$branch"; fi
        git -C "$dst" pull --ff-only || true
    else
        echo ">> clone $dst"
        if [ -n "$branch" ]; then
            git clone --branch "$branch" "$url" "$dst"
        else
            git clone "$url" "$dst"
        fi
    fi
}

# =========================================================
# 1) Dependances
# =========================================================
sudo apt update
sudo apt install -y build-essential cmake git pkg-config libfftw3-dev \
    libglfw3-dev libvolk-dev libzstd-dev mesa-common-dev libgl-dev \
    librtlsdr-dev libhackrf-dev libairspy-dev libairspyhf-dev \
    libusb-1.0-0-dev libiio-dev libad9361-dev \
    libbladerf-dev liblimesuite-dev libsoapysdr-dev libcodec2-dev \
    portaudio19-dev libfaad-dev libopus-dev libfdk-aac-dev \
    libsndfile1-dev libspeexdsp-dev \
    libopenjp2-7-dev libstb-dev libxkbcommon-dev \
    libxcb-image0-dev libxcb-util1 libxcb-cursor-dev \
    libxcb-randr0-dev libxcb-keysyms1-dev \
    libglew-dev nlohmann-json3-dev \
    libglib2.0-dev zlib1g-dev libxml2-dev \
    libpulse-dev liblapack-dev \
    socat rtl-sdr wget libncurses-dev libncurses6 \
    libjansson-dev libsqlite3-dev libzmq3-dev libprotobuf-c-dev

# =========================================================
# 2) SDRPlusPlus + dependances in-tree
# =========================================================
cd ~
clone_or_update https://github.com/F4JTV/SDRPlusPlus.git "$HOME/SDRPlusPlus"

# welle.io (DAB)
clone_or_update https://github.com/F4JTV/welle.io.git \
    "$HOME/SDRPlusPlus/decoder_modules/dab_decoder/welle.io"

# Dream (DRM) 
clone_or_update https://github.com/F4JTV/dream.git \
    "$HOME/SDRPlusPlus/decoder_modules/drm_decoder/dream"

# =========================================================
# 3) mbelib + dsd-fme (DSD)
# =========================================================
clone_or_update https://github.com/F4JTV/mbelib.git "$HOME/mbelib"
cd ~/mbelib
cmake -S . -B build
cmake --build build -j"$(nproc)"
sudo cmake --install build
sudo ldconfig

# dsd-fme 
clone_or_update https://github.com/F4JTV/dsd-fme.git "$HOME/dsd-fme"
cd ~/dsd-fme
cmake -S . -B build
cmake --build build -j"$(nproc)"
sudo cmake --install build
sudo ldconfig

# =========================================================
# 4) rtl_433 (RTL-433)
# =========================================================
clone_or_update https://github.com/F4JTV/rtl_433.git "$HOME/rtl_433"
cd ~/rtl_433
cmake -S . -B build \
      -DENABLE_RTLSDR=OFF -DENABLE_SOAPYSDR=OFF -DENABLE_OPENSSL=OFF \
      -DBUILD_TESTING=OFF \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DCMAKE_C_FLAGS="-fPIC"
cmake --build build --target r_433 -j"$(nproc)"

# =========================================================
# 5) SDR Map (Django)
# =========================================================
cd ~/SDRPlusPlus/misc_modules/sdr_map_launcher/sdr_map
python3 -m pip install --user --break-system-packages -r ./requirements.txt
python3 ./manage.py migrate

# =========================================================
# 6) libacars + dumpvdl2 (VDL2)
# =========================================================
clone_or_update https://github.com/F4JTV/libacars.git "$HOME/libacars"
cd ~/libacars
cmake -S . -B build
cmake --build build -j"$(nproc)"
sudo cmake --install build
sudo ldconfig

clone_or_update https://github.com/F4JTV/dumpvdl2.git "$HOME/dumpvdl2"
cd ~/dumpvdl2
cmake -S . -B build
cmake --build build -j"$(nproc)"
sudo cmake --install build
sudo ldconfig

# =========================================================
# 7) Compilation + installation SDR++ et modules
# =========================================================
cd ~/SDRPlusPlus
cmake -S . -B build -DRTL_433_ROOT="$HOME/rtl_433"
cmake --build build -j"$(nproc)"
sudo cmake --install build

sudo setcap cap_sys_time+ep $(which sdrpp)

echo
echo ">> Termine. SDR++ et tous les modules sont installes."
