#!/bin/bash
# OpenX6X00 - RADE V1 for the RADE app (APP > Modes > RADE): freedv/rade_c at the pinned commit and the Opus fork
# with FARGAN (the vocoder) and the LPCNet feature encoder at the commit rade_c pins, as static libraries for the GUI.
# rade_c's own CMake builds a shared library and downloads Opus inside it; this script does the same steps for a
# static build with the radios' cross compiler, each download pinned and checked.
#
#   CC=<the radios' gcc> tools/rade/build.sh    the radios' (ARMv7, Cortex-A7 with NEON); prints the prefix on stdout,
#                                               and nothing else
#   tools/rade/build.sh                         the same for this PC (CC, or gcc), for tools/freedv_test.sh and
#                                               tools/freedv_tx_test.sh
#
# The prefix: include/rade and include/opus (the headers, Opus's in the tree they expect), lib/librade.a and
# lib/libopus.a, and doc/ with rade_c's licence (BSD-2-Clause) and Opus's (BSD-3-Clause). The GUI's CMake takes it as
# -DRADE_PREFIX=<prefix>, links the two libraries and installs doc/ in /usr/share/x6100/licenses/rade.
# buildroot/local-build.sh runs it (in the build container and on the GitHub runner) with CC set to the buildroot
# tree's own cross gcc and that tree's host/bin first on the PATH: Opus is configured with autotools, which the
# container has not, and buildroot has its own.
#
# V1 only: rade_api.c refers to V2 too, whose sources would bring V2's networks and their weights into the GUI, which
# opens V1 only. rade_v2_none.c here stands in for them (a rade_open() with RADE_MODE_V2 fails).
#
# Everything is kept in $HOME/.cache/openx6x00/rade: the two source tarballs (GitHub's archives of the pinned
# commits), the DNN model Opus's autogen.sh unpacks (media.xiph.org; its SHA-256 checked here, and again by Opus) and
# the prefix, under a name made of the two commits, the target and the hash of this script, of rade_v2_none.c and of
# the compiler's version. A second run with nothing changed prints that path and downloads nothing.
# Needs curl or wget, autoconf, automake, libtool (libtoolize), m4, perl, make and patch.
#
# rade_c is BSD-2-Clause, Opus BSD-3-Clause.
set -eo pipefail
exec 3>&1 1>&2          # stdout is the prefix only: everything else goes to stderr

HERE=$(cd "$(dirname "$0")" && pwd)
# rade_c's main of 7 October 2026. GitHub's tarballs of the commits (git archive): each SHA-256 is the tar's in it,
# which gzip's own bytes do not change
RADE_C_COMMIT=c8a3dc156045cae2cd251e1a4be0c304c9ddf2f9
RADE_C_URL=https://codeload.github.com/freedv/rade_c/tar.gz/$RADE_C_COMMIT
RADE_C_TAR_SHA256=a0f42f79b56795511d0e868421298a2b03500d52eda98ddc1af6cd2bbeb3bed1
# The Opus commit rade_c's cmake/BuildOpus.cmake builds
OPUS_COMMIT=940d4e5af64351ca8ba8390df3f555484c567fbb
OPUS_URL=https://codeload.github.com/xiph/opus/tar.gz/$OPUS_COMMIT
OPUS_TAR_SHA256=8d74a1cf71d1fe423f7a96eaa2c0a6f82cf03ff88b372bccb19563e253ad934f
# The DNN model (the networks' weights as C sources) that commit's autogen.sh fetches with dnn/download_model.sh,
# named by its SHA-256
OPUS_MODEL_SHA256=4ed9445b96698bad25d852e912b41495ddfa30c8dbc8a55f9cde5826ed793453
OPUS_MODEL=opus_data-$OPUS_MODEL_SHA256.tar.gz
OPUS_MODEL_URL=https://media.xiph.org/opus/models/$OPUS_MODEL

[ $# = 0 ] || { echo "usage: [CC=<cross gcc>] tools/rade/build.sh"; exit 2; }
CC=${CC:-gcc}
command -v "$CC" > /dev/null || { echo "tools/rade/build.sh: no $CC"; exit 1; }
TARGET=$("$CC" -dumpmachine)
case $TARGET in
    arm*-linux-gnueabihf)
        # The A33: Cortex-A7, hard float, NEON (the GUI's own flags, CMakeLists.txt); Opus's DNN code has NEON paths
        ARCH_FLAGS="-mtune=cortex-a7 -mcpu=cortex-a7 -mfloat-abi=hard -mfpu=neon-vfpv4"
        CROSS=1 ;;
    *)  ARCH_FLAGS=""
        CROSS=0 ;;
esac
# The binutils next to the compiler (arm-...-gcc -> arm-...-ar), or this PC's
BINU=${CC%gcc}
[ "$BINU" != "$CC" ] && command -v "${BINU}ar" > /dev/null || BINU=""
AR=${BINU}ar; RANLIB=${BINU}ranlib; NM=${BINU}nm; STRIP=${BINU}strip
for t in "$AR" "$RANLIB" "$NM" "$STRIP" autoreconf autoconf automake libtoolize m4 perl make patch tar gunzip sha256sum; do
    command -v "$t" > /dev/null || { echo "tools/rade/build.sh: $t is needed"; exit 1; }
done
command -v curl > /dev/null || command -v wget > /dev/null || { echo "tools/rade/build.sh: curl or wget is needed"; exit 1; }

CACHE=$HOME/.cache/openx6x00/rade
DL=$CACHE/dl
KEY=$( (cat "$HERE/build.sh" "$HERE/rade_v2_none.c"; "$CC" --version | head -1; echo "$TARGET") | sha256sum | cut -c1-12)
PREFIX=$CACHE/prefix-${RADE_C_COMMIT:0:8}-opus${OPUS_COMMIT:0:8}-$TARGET-$KEY
if [ -f "$PREFIX/lib/librade.a" ] && [ -f "$PREFIX/lib/libopus.a" ] && [ -f "$PREFIX/doc/opus-COPYING" ]; then
    echo "== RADE V1, rade_c ${RADE_C_COMMIT:0:8}, Opus ${OPUS_COMMIT:0:8} ($TARGET, $KEY): built already"
    echo "$PREFIX" >&3
    exit 0
fi
mkdir -p "$DL"

sha() {     # FILE [tar]: its SHA-256, or with tar the SHA-256 of the tar in the gzip FILE
    if [ "$2" = tar ]; then gunzip -c "$1" 2> /dev/null; else cat "$1"; fi | sha256sum | cut -d' ' -f1
}

fetch() {   # URL FILE SHA256 [tar]: downloaded once, kept only with the pinned hash (with tar: of the tar in it)
    [ -f "$2" ] && [ "$(sha "$2" $4)" = "$3" ] && return 0
    echo "== $1"
    if command -v curl > /dev/null; then
        curl -fsSL --retry 3 -o "$2.tmp.$$" "$1"
    else
        wget -q -O "$2.tmp.$$" "$1"
    fi
    [ "$(sha "$2.tmp.$$" $4)" = "$3" ] \
        || { echo "tools/rade/build.sh: $1 does not have the pinned SHA-256"; rm -f "$2.tmp.$$"; exit 1; }
    mv -f "$2.tmp.$$" "$2"
}

fetch "$RADE_C_URL" "$DL/rade_c-$RADE_C_COMMIT.tar.gz" $RADE_C_TAR_SHA256 tar
fetch "$OPUS_URL" "$DL/opus-$OPUS_COMMIT.tar.gz" $OPUS_TAR_SHA256 tar
fetch "$OPUS_MODEL_URL" "$DL/$OPUS_MODEL" $OPUS_MODEL_SHA256

START=$SECONDS
W=$(mktemp -d "$CACHE/work.XXXXXX")
trap 'rm -rf "$W"' EXIT     # a build that fails leaves nothing in the cache
cd "$W"

echo "== rade_c $RADE_C_COMMIT"
# The sources and the licence only: the tarball has some 45 MB of recordings besides
tar xzf "$DL/rade_c-$RADE_C_COMMIT.tar.gz" rade_c-$RADE_C_COMMIT/src rade_c-$RADE_C_COMMIT/LICENSE
mv rade_c-$RADE_C_COMMIT rade_c

echo "== Opus $OPUS_COMMIT (FARGAN, the LPCNet encoder), patched as rade_c does"
tar xzf "$DL/opus-$OPUS_COMMIT.tar.gz"
mv opus-$OPUS_COMMIT opus
cd opus
grep -q "download_model.sh \"$OPUS_MODEL_SHA256\"" autogen.sh \
    || { echo "tools/rade/build.sh: Opus's autogen.sh does not want the model $OPUS_MODEL_SHA256"; exit 1; }
# In place, autogen.sh's dnn/download_model.sh downloads nothing; it checks the SHA-256 and unpacks it
ln -s "$DL/$OPUS_MODEL" .
patch -s dnn/nnet.h < ../rade_c/src/opus-nnet.h.diff
patch -s dnn/nnet.c < ../rade_c/src/opus-nnet.c.diff
./autogen.sh > autogen.log 2>&1 || { tail -20 autogen.log; exit 1; }
# rade_c's configure options. Without Ne10 (its FFT): configure takes it when the sysroot has it, as a buildroot
# tree has once it built it, and libopus.a would then need it too. Each function and datum in a section of its own:
# the GUI's link (--gc-sections) keeps only what RADE uses
CFG=(--with-pic --enable-osce --enable-dred --disable-shared --disable-doc --disable-extra-programs --without-NE10)
[ $CROSS = 1 ] && CFG+=(--host="$TARGET")
./configure "${CFG[@]}" CC="$CC" AR="$AR" RANLIB="$RANLIB" \
    CFLAGS="-O3 $ARCH_FLAGS -ffunction-sections -fdata-sections" > configure.log 2>&1 || { tail -20 configure.log; exit 1; }
! grep -q "^#define HAVE_ARM_NE10" config.h || { echo "tools/rade/build.sh: Opus is configured with Ne10"; exit 1; }
make -j"$(nproc)" > make.log 2>&1 || { tail -20 make.log; exit 1; }
cd ..

echo "== librade.a: RADE V1 (rade_v2_none.c for V2)"
INC=(-I rade_c/src -I opus -I opus/dnn -I opus/celt -I opus/include)
CFLAGS=(-O3 $ARCH_FLAGS -fPIC -std=gnu11 -ffunction-sections -fdata-sections -DIS_BUILDING_RADE_API=1 "${INC[@]}")
mkdir obj
cp "$HERE/rade_v2_none.c" rade_c/src/
# The two weight files are 24 MB of C each: compiled side by side
PIDS=()
for s in rade_api rade_enc rade_dec rade_enc_data rade_dec_data rade_dsp rade_ofdm rade_bpf rade_acq rade_tx rade_rx \
         rade_v2_none; do
    "$CC" "${CFLAGS[@]}" -c rade_c/src/$s.c -o obj/$s.o &
    PIDS+=($!)
done
for p in "${PIDS[@]}"; do
    wait $p || { echo "tools/rade/build.sh: a RADE source did not compile"; wait; exit 1; }
done
"$AR" rcs librade.a obj/*.o

if [ $CROSS = 1 ]; then
    # A 32-bit little-endian ELF (7f 'E' 'L' 'F' 01 01) for ARM (e_machine 0x28, bytes 18-19)
    ELF=$(od -An -tx1 -N20 obj/rade_api.o | tr -d ' \n')
    [ "${ELF:0:12}" = 7f454c460101 ] && [ "${ELF:36:4}" = 2800 ] \
        || { echo "tools/rade/build.sh: librade.a is not 32-bit ARM"; exit 1; }
fi

echo "== link check: what the GUI calls (src/freedv_rx.c, src/freedv_tx.c), as the GUI links it"
cat > check.c << 'EOF'
#include <stdio.h>
#include "rade_api.h"
#include "fargan.h"
#include "lpcnet.h"
#include "cpu_support.h"
void *volatile used[] = {
    (void *)rade_initialize, (void *)rade_open, (void *)rade_close, (void *)rade_nin, (void *)rade_nin_max,
    (void *)rade_rx, (void *)rade_sync, (void *)rade_freq_offset, (void *)rade_snrdB_3k_est, (void *)rade_tx,
    (void *)rade_tx_eoo, (void *)rade_n_tx_out, (void *)rade_n_tx_eoo_out, (void *)rade_n_features_in_out,
    (void *)rade_n_eoo_bits, (void *)rade_tx_set_eoo_bits, (void *)fargan_init, (void *)fargan_cont,
    (void *)fargan_synthesize, (void *)lpcnet_encoder_create, (void *)lpcnet_encoder_destroy,
    (void *)lpcnet_compute_single_frame_features,
};
int main(void)
{
    struct rade *v1, *v2;
    for (unsigned i = 0; i < sizeof(used) / sizeof(used[0]); i++)
        if (!used[i]) return 2;
    rade_initialize();
    v1 = rade_open("", RADE_VERBOSE_0);
    v2 = rade_open("", RADE_MODE_V2 | RADE_VERBOSE_0);
    printf("arch %d, V1 %s, V2 %s\n", opus_select_arch(), v1 ? "opens" : "does not open", v2 ? "opens" : "does not open");
    return !(v1 && !v2);
}
EOF
# As src/CMakeLists.txt links it: all of libopus.a (its references must all resolve), the unused dropped
"$CC" "${CFLAGS[@]}" -fPIE -pie check.c -o check -Wl,--gc-sections \
    librade.a -Wl,--whole-archive opus/.libs/libopus.a -Wl,--no-whole-archive -lm
"$STRIP" -o check.stripped check
echo "   RADE and Opus in a program: $(wc -c < check.stripped) bytes stripped"
if [ $CROSS = 0 ]; then
    ./check 2> /dev/null || { echo "tools/rade/build.sh: V1 must open, V2 not"; exit 1; }
fi
# Nothing of V2's own code or weights in the library
V2=$("$NM" librade.a | grep -E " [TDR] (radeencv2_arrays|radedecv2_arrays|radesync_arrays|rade_frame_sync|rade_v2_ofdm_)" || true)
[ -z "$V2" ] || { echo "tools/rade/build.sh: V2 is in librade.a: $V2"; exit 1; }

echo "== install"
D=$W/prefix
mkdir -p "$D/lib" "$D/include/rade" "$D/include/opus" "$D/doc"
cp librade.a "$D/lib/"
cp opus/.libs/libopus.a "$D/lib/"
cp rade_c/src/*.h "$D/include/rade/"
# The headers fargan.h and lpcnet.h pull in, with the tree they expect, and the configure's config.h
(cd opus && tar cf - $(find dnn celt include silk -name '*.h') config.h) | tar xf - -C "$D/include/opus"
cp rade_c/LICENSE "$D/doc/rade_c-LICENSE"
cp opus/COPYING "$D/doc/opus-COPYING"
rm -rf "$PREFIX"
mv "$D" "$PREFIX"
echo "== $(du -sh "$PREFIX/lib" | cut -f1) of libraries in $PREFIX, built in $((SECONDS - START)) s"
echo "$PREFIX" >&3
