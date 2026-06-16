#!/usr/bin/env bash
# Build a multi-pass Meteor LRPT test CS8 for HackRF (72k OQPSK, M2-3 style),
# with several DIFFERENT images separated by dead air, so you can test the
# module's "Auto-save PNG + reset between passes" feature.
#
# Usage:
#   ./make_test_cs8.sh                              # 3 distinct procedural images
#   ./make_test_cs8.sh img1.png img2.png img3.png   # your own real images (one per pass)
set -e
cd "$(dirname "$0")"

# ALWAYS rebuild so the generator is never a stale binary from an older version.
./build_gen.sh

OUT=meteor_test_multi.cs8
SR=2304000     # 2.304 Msps = 32 x 72k (integer sps, >=2 Msps for HackRF)
GAP=12         # seconds of dead air between passes (set the module's "LOS gap" lower, e.g. 8)
LINES=40       # procedural image height in 8-px groups
MAXLINES=120   # cap loaded-image height (~960 px) to keep the file size sane

if [ "$#" -ge 1 ]; then
    # Build a comma-separated list portably (no IFS tricks).
    IMAGES=""
    for f in "$@"; do
        if [ ! -f "$f" ]; then echo "ERROR: image '$f' not found"; exit 1; fi
        if [ -z "$IMAGES" ]; then IMAGES="$f"; else IMAGES="$IMAGES,$f"; fi
    done
    echo "Using your images: $IMAGES"
    ./meteor_lrpt_gen --mode m2x --images "$IMAGES" --maxlines "$MAXLINES" \
        --gap "$GAP" --samplerate "$SR" -o "$OUT"
else
    echo "No images given -> 3 distinct procedural scenes"
    ./meteor_lrpt_gen --mode m2x --passes 3 --lines "$LINES" \
        --gap "$GAP" --samplerate "$SR" -o "$OUT"
fi

echo
echo "Done: $OUT"
echo "Transmit (dummy load / attenuator ONLY, never a live antenna):"
echo "  hackrf_transfer -t $OUT -f 137900000 -s $SR -x 10 -a 0"
echo
echo "In the module: select 'Meteor-M2-3 (72k OQPSK)', tick 'Decode LRPT (live)',"
echo "tick 'Auto-save PNG + reset between passes' and set 'LOS gap (s)' to ~8"
echo "(must be a bit LESS than the ${GAP}s gap). Each pass is saved as its own PNG."
