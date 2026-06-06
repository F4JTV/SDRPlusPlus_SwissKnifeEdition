#!/usr/bin/env bash
#
# apply_to_sdrpp.sh - integrate the VDL Mode 2 decoder module into an SDR++ tree.
#
# Usage:
#   ./apply_to_sdrpp.sh /path/to/SDRPlusPlus
#
# Idempotent: safe to run multiple times. Performs three edits:
#   1. copies this module into decoder_modules/vdl2_decoder/
#   2. adds OPT_BUILD_VDL2_DECODER option + add_subdirectory to root CMakeLists.txt
#   3. registers vdl2_decoder.so in the default module list in core/src/core.cpp
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDRPP_ROOT="${1:-}"

if [ -z "$SDRPP_ROOT" ]; then
    echo "Usage: $0 /path/to/SDRPlusPlus" >&2
    exit 1
fi
if [ ! -f "$SDRPP_ROOT/CMakeLists.txt" ] || [ ! -f "$SDRPP_ROOT/core/src/core.cpp" ]; then
    echo "Error: '$SDRPP_ROOT' does not look like an SDR++ source tree." >&2
    exit 1
fi

CMAKE="$SDRPP_ROOT/CMakeLists.txt"
CORE="$SDRPP_ROOT/core/src/core.cpp"
DEST="$SDRPP_ROOT/decoder_modules/vdl2_decoder"

# --- 1. copy the module ---------------------------------------------------
echo "==> Copying module to $DEST"
mkdir -p "$DEST"
cp -r "$SCRIPT_DIR/CMakeLists.txt" "$SCRIPT_DIR/src" "$DEST/"
[ -f "$SCRIPT_DIR/README.md" ] && cp "$SCRIPT_DIR/README.md" "$DEST/"

# --- 2. patch root CMakeLists.txt -----------------------------------------
if grep -q "OPT_BUILD_VDL2_DECODER" "$CMAKE"; then
    echo "==> CMakeLists.txt already patched, skipping"
else
    echo "==> Patching $CMAKE"
    # Add the option right after the pager decoder option.
    awk '
        { print }
        /option\(OPT_BUILD_PAGER_DECODER/ && !did_opt {
            print "option(OPT_BUILD_VDL2_DECODER \"Build the VDL Mode 2 decoder module (no dependencies required)\" ON)"
            did_opt=1
        }
        /endif \(OPT_BUILD_PAGER_DECODER\)/ && !did_subdir {
            print ""
            print "if (OPT_BUILD_VDL2_DECODER)"
            print "add_subdirectory(\"decoder_modules/vdl2_decoder\")"
            print "endif (OPT_BUILD_VDL2_DECODER)"
            did_subdir=1
        }
    ' "$CMAKE" > "$CMAKE.tmp" && mv "$CMAKE.tmp" "$CMAKE"
fi

# --- 3. patch core.cpp default module list --------------------------------
if grep -q "vdl2_decoder.so" "$CORE"; then
    echo "==> core.cpp already patched, skipping"
else
    echo "==> Patching $CORE"
    awk '
        { print }
        /conf\["modules"\]\[modCount\+\+\] = "meteor_demodulator.so";/ && !done {
            indent = $0; sub(/[^ ].*/, "", indent)
            print indent "core::configManager.conf[\"modules\"][modCount++] = \"vdl2_decoder.so\";"
            done=1
        }
    ' "$CORE" > "$CORE.tmp" && mv "$CORE.tmp" "$CORE"
fi

echo ""
echo "Done. Now rebuild SDR++:"
echo "  cd \"$SDRPP_ROOT\" && mkdir -p build && cd build"
echo "  cmake .. -DOPT_BUILD_VDL2_DECODER=ON && make -j\$(nproc) && sudo make install"
