#!/bin/bash
# OpenX6X00 - SVXConnect (the SvxLink reflector client, https://github.com/Guru-RF/SVXConnect-CLI) for the
# SVXConnect app: one ARMv7 binary for the X6100 and the X6200, built from the pinned tag and commit with our
# patches (patches/*.patch, in order) on top: the audio through the GUI instead of a sound card (make AUDIO=pipe),
# svxconnect --stdio (the GUI's socket as its stdin and stdout: commands in, events and audio out) and
# --cert-status, the fake reflector as a program for the tests, and no terminal interface (make TUI=0, no ncurses).
# See UPSTREAM.md.
#
#   CC=<the radios' gcc> tools/svxconnect/build.sh    prints the binary's path on stdout, and nothing else
#   tools/svxconnect/build.sh                         the same for this PC (CC, or gcc), with build/fakerefl beside
#                                                     it, for tools/svx's tests and the PC build of the GUI
#
# buildroot/local-build.sh runs it (in the build container and on the GitHub runner) with CC set to the buildroot
# tree's own cross gcc and gives the path to the GUI's CMake (-DSVXCONNECT_BINARY), which installs it as
# /usr/bin/svxconnect and the doc/ beside it (its licence and its notices) in /usr/share/x6100/licenses/svxconnect.
# It links the tree's OpenSSL and Opus (both in the image already), found with pkg-config in the compiler's sysroot,
# which local-build.sh has buildroot build first.
#
# Everything is kept in $HOME/.cache/openx6x00/svxconnect: the binary, under a name made of the tag, the target and
# the hash of the patches, of this script and of the compiler's version. A second run with nothing changed prints
# that path and clones nothing. A native build also runs SVXConnect's own tests (make test).
# Needs git, make and pkg-config, and for a native build OpenSSL's and Opus's headers.
#
# Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
set -eo pipefail
exec 3>&1 1>&2          # stdout is the binary's path only: everything else goes to stderr

HERE=$(cd "$(dirname "$0")" && pwd)
SVX_TAG=v0.1.4
SVX_COMMIT=578e5de47c228576a86d00b487cb03478ab5c6e4
SVX_REPO=https://github.com/Guru-RF/SVXConnect-CLI

[ $# = 0 ] || { echo "usage: [CC=<cross gcc>] tools/svxconnect/build.sh"; exit 2; }
CC=${CC:-gcc}
command -v "$CC" > /dev/null || { echo "tools/svxconnect/build.sh: no $CC"; exit 1; }
TARGET=$("$CC" -dumpmachine)
case $TARGET in
    arm*-linux-gnueabihf)
        # The radios' processor: Cortex-A7, hard float, NEON (the GUI's own flags, CMakeLists.txt)
        ARCH_FLAGS="-mtune=cortex-a7 -mcpu=cortex-a7 -mfloat-abi=hard -mfpu=neon-vfpv4"
        CROSS=1 ;;
    *)  ARCH_FLAGS=""
        CROSS=0 ;;
esac
BINU=${CC%gcc}
[ "$BINU" != "$CC" ] && command -v "${BINU}strip" > /dev/null || BINU=""
STRIP=${BINU}strip
for t in git make pkg-config "$STRIP" sha256sum od; do
    command -v "$t" > /dev/null || { echo "tools/svxconnect/build.sh: $t is needed"; exit 1; }
done

if [ $CROSS = 1 ]; then
    # OpenSSL and Opus as buildroot built them: the .pc files of the compiler's sysroot, their paths in it
    SYSROOT=$("$CC" -print-sysroot)
    [ -d "$SYSROOT/usr/lib/pkgconfig" ] || { echo "tools/svxconnect/build.sh: no sysroot for $CC"; exit 1; }
    export PKG_CONFIG_LIBDIR=$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig PKG_CONFIG_SYSROOT_DIR=$SYSROOT
fi
for m in opus openssl; do
    pkg-config --exists $m || { echo "tools/svxconnect/build.sh: no $m for $TARGET (pkg-config)"; exit 1; }
done

PATCHES=("$HERE"/patches/*.patch)
[ -e "${PATCHES[0]}" ] || PATCHES=()
KEY=$( (cat "${PATCHES[@]}" "$HERE/build.sh"; "$CC" --version | head -1; echo "$TARGET") | sha256sum | cut -c1-12)
CACHE=$HOME/.cache/openx6x00/svxconnect
OUT=$CACHE/svxconnect-$SVX_TAG-$TARGET-$KEY
if [ -x "$OUT/svxconnect" ] && [ -f "$OUT/doc/LICENSE" ] && { [ $CROSS = 1 ] || [ -x "$OUT/fakerefl" ]; }; then
    echo "== SVXConnect $SVX_TAG ($TARGET, $KEY): built already"
    echo "$OUT/svxconnect" >&3
    exit 0
fi
mkdir -p "$CACHE"

START=$SECONDS
W=$(mktemp -d "$CACHE/work.XXXXXX")
trap 'rm -rf "$W"' EXIT     # a build that fails leaves nothing in the cache
echo "== SVXConnect $SVX_TAG"
git -c advice.detachedHead=false clone -q --depth 1 --branch "$SVX_TAG" "$SVX_REPO" "$W/src"
cd "$W/src"
HEAD=$(git rev-parse HEAD)
[ "$HEAD" = "$SVX_COMMIT" ] || { echo "tools/svxconnect/build.sh: $SVX_TAG is $HEAD, not the pinned $SVX_COMMIT"; exit 1; }
for p in "${PATCHES[@]}"; do
    echo "   $(basename "$p")"
    git apply "$p"
done

echo "== svxconnect for $TARGET: the audio through the GUI (AUDIO=pipe), no terminal interface (TUI=0)"
MK=(BUILD=build AUDIO=pipe TUI=0 CC="$CC"
    CFLAGS="-O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 $ARCH_FLAGS")
make -j"$(nproc)" "${MK[@]}" all > make.log 2>&1 || { tail -30 make.log; exit 1; }
if [ $CROSS = 1 ]; then
    # A 32-bit little-endian ELF (7f 'E' 'L' 'F' 01 01) for ARM (e_machine 0x28, bytes 18-19)
    ELF=$(od -An -tx1 -N20 build/svxconnect | tr -d ' \n')
    [ "${ELF:0:12}" = 7f454c460101 ] && [ "${ELF:36:4}" = 2800 ] \
        || { echo "tools/svxconnect/build.sh: the result is not a 32-bit ARM ELF"; exit 1; }
else
    echo "== SVXConnect's own tests, and the fake reflector for ours"
    make -j"$(nproc)" "${MK[@]}" test fakerefl > test.log 2>&1 || { tail -30 test.log; exit 1; }
    grep -E "checks, [0-9]+ failed|passed" test.log | sed 's/^/   /'
fi

D=$W/out
mkdir -p "$D/doc"
"$STRIP" -o "$D/svxconnect" build/svxconnect
[ $CROSS = 1 ] || cp build/fakerefl "$D/"
cp LICENSE THIRD-PARTY-NOTICES "$D/doc/"
rm -rf "$OUT"
mv "$D" "$OUT"
echo "== $(wc -c < "$OUT/svxconnect") bytes, built in $((SECONDS - START)) s, sha256 $(sha256sum "$OUT/svxconnect" | cut -d' ' -f1)"
echo "$OUT/svxconnect" >&3
