#!/bin/bash
# OpenX6X00 - RADE V1 for the RADE app (APP > Modes > RADE): freedv/rade_c at the pinned commit and
# the Opus fork with FARGAN (the vocoder), as static libraries for the GUI. rade_c's own CMake builds
# a shared library and runs the downloads inside it; this script does the same steps for a static
# build with the radio's cross compiler.
#
#   RADE_HOST=arm-buildroot-linux-gnueabihf tools/rade/build.sh   cross build (the compiler is
#                                                                 $RADE_HOST-gcc on the PATH)
#   tools/rade/build.sh                                           for this PC (tests)
#
# Prints the prefix on stdout, and nothing else: include/ (rade/, opus/...) and lib/librade.a, libopus.a.
# Give it to the GUI's CMake as -DRADE_PREFIX=<prefix>. Everything is kept in $HOME/.cache/openx6x00/rade.
#
# Needs the network once (the Opus archive from GitHub, and the FARGAN model that Opus's autogen.sh
# fetches from media.xiph.org, SHA-256 checked by Opus) and autoconf, automake, libtool, wget, unzip.
# NOT TESTED for ARM; the native build is what tools/freedv_test.c was run against.
#
# rade_c is BSD-2-Clause, Opus BSD-3-Clause.
set -eo pipefail
exec 3>&1 1>&2          # stdout is the prefix only

RADE_C_REPO=https://github.com/freedv/rade_c
RADE_C_COMMIT=c8a3dc156045cae2cd251e1a4be0c304c9ddf2f9   # 2026-10-07
OPUS_COMMIT=940d4e5af64351ca8ba8390df3f555484c567fbb   # as rade_c/cmake/BuildOpus.cmake
OPUS_URL=https://github.com/xiph/opus/archive/$OPUS_COMMIT.zip

HERE=$(cd "$(dirname "$0")" && pwd)
RADE_HOST=${RADE_HOST:-}
if [ -n "$RADE_HOST" ]; then
    CC=${RADE_HOST}-gcc; AR=${RADE_HOST}-ar
    # The A33: Cortex-A7 with NEON; Opus's DNN code has NEON paths
    ARCH_FLAGS=${RADE_ARCH_FLAGS:--mtune=cortex-a7 -mcpu=cortex-a7 -mfloat-abi=hard -mfpu=neon-vfpv4}
    TAG=$RADE_HOST
else
    CC=${CC:-gcc}; AR=${AR:-ar}
    ARCH_FLAGS=${RADE_ARCH_FLAGS:-}
    TAG=native
fi
command -v "$CC" > /dev/null || { echo "tools/rade/build.sh: $CC not found"; exit 1; }
for t in autoconf automake libtoolize wget unzip patch; do
    command -v $t > /dev/null || { echo "tools/rade/build.sh: $t is needed"; exit 1; }
done

CACHE=$HOME/.cache/openx6x00/rade
KEY=$(cat "$HERE/build.sh" | sha256sum | cut -c1-10)
PREFIX=$CACHE/prefix-${RADE_C_COMMIT:0:8}-$TAG-$KEY
if [ -e "$PREFIX/lib/librade.a" ] && [ -e "$PREFIX/lib/libopus.a" ]; then
    echo "== RADE ${RADE_C_COMMIT:0:8} ($TAG): built already"
    echo "$PREFIX" >&3
    exit 0
fi

mkdir -p "$CACHE"
W=$(mktemp -d "$CACHE/work.XXXXXX")
trap 'rm -rf "$W"' EXIT
cd "$W"

echo "== rade_c $RADE_C_COMMIT"
git clone -q "$RADE_C_REPO" rade_c
git -C rade_c -c advice.detachedHead=false checkout -q "$RADE_C_COMMIT"
[ "$(git -C rade_c rev-parse HEAD)" = "$RADE_C_COMMIT" ] || { echo "tools/rade/build.sh: rade_c is not at $RADE_C_COMMIT"; exit 1; }

echo "== Opus $OPUS_COMMIT (with FARGAN), patched as rade_c does"
wget -q -O opus.zip "$OPUS_URL"
unzip -q opus.zip
mv opus-$OPUS_COMMIT opus
cd opus
patch -s dnn/nnet.h < ../rade_c/src/opus-nnet.h.diff
patch -s dnn/nnet.c < ../rade_c/src/opus-nnet.c.diff
./autogen.sh > autogen.log 2>&1 || { tail -20 autogen.log; exit 1; }
CFG=(--with-pic --enable-osce --enable-dred --disable-shared --disable-doc --disable-extra-programs)
[ -n "$RADE_HOST" ] && CFG+=(--host="$RADE_HOST")
./configure "${CFG[@]}" CC="$CC" AR="$AR" CFLAGS="-O3 $ARCH_FLAGS" > configure.log 2>&1 || { tail -20 configure.log; exit 1; }
make -j"$(nproc)" > make.log 2>&1 || { tail -20 make.log; exit 1; }
cd ..

echo "== librade.a (V1 and V2 sources: rade_api.c refers to both)"
INC=(-I rade_c/src -I opus -I opus/dnn -I opus/celt -I opus/include)
mkdir -p obj
SRCS=(rade_api rade_enc rade_dec rade_enc_data rade_dec_data rade_dsp rade_ofdm rade_bpf rade_acq rade_tx rade_rx
      rade_enc_v2 rade_dec_v2 rade_sync rade_enc_v2_data rade_dec_v2_data rade_sync_data
      rade_v2_ofdm rade_tx_v2 rade_rx_v2)
for s in "${SRCS[@]}"; do
    $CC -O3 $ARCH_FLAGS -fPIC -std=gnu11 -ffunction-sections -fdata-sections "${INC[@]}" -c rade_c/src/$s.c -o obj/$s.o
done
$AR rcs librade.a obj/*.o

echo "== install"
D=$W/prefix
mkdir -p "$D/lib" "$D/include/rade" "$D/include/opus"
cp librade.a "$D/lib/"
cp opus/.libs/libopus.a "$D/lib/"
cp rade_c/src/*.h "$D/include/rade/"
# The headers fargan.h and lpcnet.h pull in, with the tree they expect
(cd opus && tar cf - $(find dnn celt include silk -name '*.h') *.h 2> /dev/null) | tar xf - -C "$D/include/opus"
mkdir -p "$(dirname "$PREFIX")"
rm -rf "$PREFIX"
mv "$D" "$PREFIX"
echo "== $(du -sh "$PREFIX/lib" | cut -f1) of libraries in $PREFIX"
echo "$PREFIX" >&3
