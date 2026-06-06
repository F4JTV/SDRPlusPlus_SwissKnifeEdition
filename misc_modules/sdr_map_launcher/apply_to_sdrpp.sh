#!/usr/bin/env bash
#
# apply_to_sdrpp.sh — drop the sdr_map_launcher module into an SDR++ source
#                     tree, register it in the root CMakeLists.txt and the
#                     core module list. Idempotent: safe to re-run.
#
# Usage:
#   ./apply_to_sdrpp.sh /path/to/SDRPlusPlus
#
set -e

SDRPP_DIR="${1:-}"
if [ -z "$SDRPP_DIR" ] || [ ! -f "$SDRPP_DIR/CMakeLists.txt" ]; then
    echo "Usage: $0 /path/to/SDRPlusPlus"
    echo "  (the directory must contain SDR++'s root CMakeLists.txt)"
    exit 1
fi

HERE="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
MOD_DIR="$SDRPP_DIR/misc_modules/sdr_map_launcher"

# 1) Copy the module sources into misc_modules/
mkdir -p "$MOD_DIR"
cp "$HERE/CMakeLists.txt" "$MOD_DIR/"
cp -r "$HERE/src" "$MOD_DIR/"
echo "  ✓ module sources copied to $MOD_DIR"

# 2) Add the build option in the root CMakeLists.txt (next to the others)
CM="$SDRPP_DIR/CMakeLists.txt"
if ! grep -q "OPT_BUILD_SDR_MAP_LAUNCHER" "$CM"; then
    # Insert after the SCHEDULER option line.
    awk '
        /^option\(OPT_BUILD_SCHEDULER/ {
            print
            print "option(OPT_BUILD_SDR_MAP_LAUNCHER \"Launcher panel for the companion sdr_map Django web server\" OFF)"
            next
        }
        { print }
    ' "$CM" > "$CM.tmp" && mv "$CM.tmp" "$CM"
    echo "  ✓ option OPT_BUILD_SDR_MAP_LAUNCHER added"
else
    echo "  · option already present"
fi

# 3) Add the add_subdirectory() block
if ! grep -q 'misc_modules/sdr_map_launcher' "$CM"; then
    awk '
        BEGIN { added = 0 }
        /^endif \(OPT_BUILD_SCHEDULER\)/ {
            print
            print ""
            print "if (OPT_BUILD_SDR_MAP_LAUNCHER)"
            print "add_subdirectory(\"misc_modules/sdr_map_launcher\")"
            print "endif (OPT_BUILD_SDR_MAP_LAUNCHER)"
            added = 1
            next
        }
        { print }
    ' "$CM" > "$CM.tmp" && mv "$CM.tmp" "$CM"
    echo "  ✓ add_subdirectory block added"
else
    echo "  · add_subdirectory already present"
fi

echo ""
echo "Now build with:"
echo "  cd $SDRPP_DIR && mkdir -p build && cd build"
echo "  cmake .. -DCMAKE_BUILD_TYPE=Release -DOPT_BUILD_SDR_MAP_LAUNCHER=ON"
echo "  make -j\$(nproc)"
echo ""
echo "The plugin will be at:"
echo "  build/misc_modules/sdr_map_launcher/sdr_map_launcher.so"
