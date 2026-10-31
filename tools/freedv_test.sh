#!/bin/bash
# Runs src/freedv_rx.c on a PC against the real RADE library (not part of the image):
#   tools/freedv_test.sh in.wav out.wav [speed]    in.wav any rate, 16-bit mono; speed 1 = real time
# Builds RADE with tools/rade/build.sh (native, cached), resamples with ffmpeg, writes out.wav (44.1 kHz).
set -eo pipefail
TOP=$(cd "$(dirname "$0")/.." && pwd)
P=$("$TOP/tools/rade/build.sh")
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
ffmpeg -loglevel error -y -i "$1" -ar 11025 -ac 1 -c:a pcm_s16le "$T/in.wav"
gcc -O2 -Wall -Wextra -DRADE_ENABLED=1 -I "$TOP/src" -I "$TOP" \
    -I "$P/include/rade" -I "$P/include/opus" -I "$P/include/opus/dnn" -I "$P/include/opus/celt" -I "$P/include/opus/include" \
    "$TOP/tools/freedv_test.c" "$TOP/src/freedv_rx.c" "$TOP/src/olivia_resamp.c" -o "$T/freedv_test" \
    "$P/lib/librade.a" -Wl,--whole-archive "$P/lib/libopus.a" -Wl,--no-whole-archive -lm -lpthread
"$T/freedv_test" "$T/in.wav" "$2" "${3:-0}"
