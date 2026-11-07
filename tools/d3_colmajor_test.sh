#!/bin/sh
# 3D waterfall, landscape: the picture drawn as columns and turned over vs drawn in rows (the commit before),
# pixel for pixel. Usage: d3_colmajor_test.sh [outdir] [base-rev]; CC=cc for a host run (default: static armv7),
# BENCH=1 to print timings, NONEON=1 for the scalar turn, BOARD_DEFS for the board (as cmake: e.g.
# -DBOARD_X6200=1); the panel size is its board.h BOARD_DISP_W/H. Runs the binaries (on the host: CC=cc);
# NORUN=1 only builds them (d3_test_old / d3_test_new OUT [bench] on the radio, then cmp the OUTs).
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-.}
base=${2:-HEAD~1}
CC=${CC:-armv7-linux-gnueabihf-gcc}
extract() {
    awk '/^#define D3_STEP/{p=1} /^\/\* The reference frequencies under the front edge/{p=0} p'
}
git -C "$here" show "$base:src/waterfall_view.c" | extract > "$out/d3_old.inc"
extract < "$here/../src/waterfall_view.c" > "$out/d3_new.inc"
grep -q d3_transpose "$out/d3_new.inc"
if grep -q d3_transpose "$out/d3_old.inc"; then echo "base already has the turn"; exit 1; fi
NEON=
[ -n "$NONEON" ] && NEON=-U__ARM_NEON
[ "$CC" = armv7-linux-gnueabihf-gcc ] && ARCH="-static -mfpu=neon -mfloat-abi=hard"
for v in old new; do
    cp "$out/d3_$v.inc" "$out/d3_extract.inc"
    $CC -O2 $ARCH $NEON $BOARD_DEFS -Wno-unused-function -I"$out" -I"$here/../src" \
        -o "$out/d3_test_$v" "$here/d3_colmajor_test.c" -lm
    [ -n "$NORUN" ] || "$out/d3_test_$v" "$out/d3_$v.sums" ${BENCH:+bench}
done
[ -n "$NORUN" ] && exit 0
cmp "$out/d3_old.sums" "$out/d3_new.sums" && echo "identical: $(wc -l < "$out/d3_new.sums") pictures"
