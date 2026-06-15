#!/usr/bin/env bash
# Build the Meteor LRPT test-signal generator.
# Run from the meteor_demodulator/tools directory.
set -e
SRC=../src
# Path to SDR++'s vendored libcorrect (adjust if your tree differs)
LC=../../../core/libcorrect
if [ ! -f "$LC/include/correct.h" ]; then
  echo "libcorrect not found at $LC — set LC to your SDR++ core/libcorrect path"; exit 1
fi
# Compile libcorrect Reed-Solomon objects once
mkdir -p /tmp/lc_objs
cc -c -O2 -I "$LC/include" "$LC"/src/reed-solomon/*.c -w
g++ -std=c++17 -O2 -w \
  -I "$SRC" -I "$SRC/lrpt" -I "$LC/include" \
  meteor_lrpt_gen.cpp \
  "$SRC"/lrpt/codings/reedsolomon.cpp \
  "$SRC"/lrpt/codings/randomization.cpp \
  "$SRC"/lrpt/codings/nrzm.cpp \
  "$SRC"/lrpt/codings/viterbi/cc_encoder.cpp \
  "$SRC"/lrpt/ccsds/ccsds.cpp \
  "$SRC"/lrpt/msumr/huffman.cpp \
  ./*.o \
  -o meteor_lrpt_gen
rm -f ./*.o
echo "Built ./meteor_lrpt_gen"
