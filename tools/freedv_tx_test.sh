#!/bin/bash
# Runs src/freedv_tx.c on a PC against the real RADE library and decodes the result with
# src/freedv_rx.c (not part of the image):
#   tools/freedv_tx_test.sh speech.wav [CALLSIGN]
# Prints the encoder's numbers, then the receiver's: sync, SNR, the callsign of the End-of-Over.
set -eo pipefail
TOP=$(cd "$(dirname "$0")/.." && pwd)
P=$("$TOP/tools/rade/build.sh")
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
INC=(-I "$TOP/src" -I "$TOP" -I "$P/include/rade" -I "$P/include/opus" -I "$P/include/opus/dnn" -I "$P/include/opus/celt" -I "$P/include/opus/include")
LIBS=("$P/lib/librade.a" -Wl,--whole-archive "$P/lib/libopus.a" -Wl,--no-whole-archive -lm -lpthread)
ffmpeg -loglevel error -y -i "$1" -ar 44100 -ac 1 -c:a pcm_s16le "$T/mic.wav"
gcc -O2 -Wall -Wextra -DRADE_ENABLED=1 "${INC[@]}" "$TOP/tools/freedv_tx_test.c" "$TOP/src/freedv_tx.c" "$TOP/src/olivia_resamp.c" -o "$T/tx" "${LIBS[@]}"
gcc -O2 -Wall -Wextra -DRADE_ENABLED=1 "${INC[@]}" "$TOP/tools/freedv_test.c" "$TOP/src/freedv_rx.c" "$TOP/src/olivia_resamp.c" -o "$T/rx" "${LIBS[@]}"
"$T/tx" "$T/mic.wav" "$T/over.wav" "${2:-TEST}"
ffmpeg -loglevel error -y -i "$T/over.wav" -af "apad=pad_dur=2" -ar 11025 -c:a pcm_s16le "$T/over11.wav"
"$T/rx" "$T/over11.wav" "$T/decoded.wav" 10
