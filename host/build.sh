#!/bin/sh
# OpenX6X00 - the GUI on this PC: builds liquid-dsp and ft8_lib once into build-host/deps,
# configures and builds the GUI with an SDL window and the stub radio, puts the default
# settings in the data directory and runs it. See host/README.md.
#
#   host/build.sh            build and run
#   host/build.sh build      build only
#   host/build.sh run        run what is built
#   host/build.sh clean      remove build-host/ (the dependencies and the data too)
#
# Copyright (c) 2026 RF.Guru
set -e
cd "$(dirname "$0")/.."
TOP=$PWD
B=$TOP/build-host
DEPS=$B/deps
LIQUID_VER=1.4.0
FT8LIB_REV=73b0db861dc41c7ca9f3dedc73ee148daa06dd0e
BOOST_VER=1_80_0

deps() {
    mkdir -p "$DEPS/src" "$DEPS/lib" "$DEPS/include/ft8lib"
    if [ ! -e "$DEPS/lib/libliquid.so" ]; then
        echo "== liquid-dsp $LIQUID_VER"
        cd "$DEPS/src"
        [ -d liquid-dsp ] || git clone -q --depth 1 --branch v$LIQUID_VER https://github.com/jgaeddert/liquid-dsp.git
        cd liquid-dsp
        ./bootstrap.sh > /dev/null && ./configure --prefix="$DEPS" > /dev/null && make -j"$(nproc)" > /dev/null && make install > /dev/null
        cd "$TOP"
    fi
    if [ ! -d "$DEPS/include/boost" ]; then
        echo "== boost $BOOST_VER headers (js8core)"
        cd "$DEPS/src"
        [ -e boost_$BOOST_VER.tar.bz2 ] || curl -fsSL -o boost_$BOOST_VER.tar.bz2 \
            "https://archives.boost.io/release/$(echo $BOOST_VER | tr _ .)/source/boost_$BOOST_VER.tar.bz2"
        tar xjf boost_$BOOST_VER.tar.bz2 --strip-components=1 -C "$DEPS/include" boost_$BOOST_VER/boost
        cd "$TOP"
    fi
    if [ ! -e "$DEPS/lib/libft8.so" ]; then
        echo "== ft8_lib $FT8LIB_REV"
        cd "$DEPS/src"
        [ -d ft8_lib ] || { git clone -q https://github.com/gdyuldin/ft8_lib.git && git -C ft8_lib checkout -q $FT8LIB_REV; }
        cd ft8_lib
        make lib CFLAGS="-O2 -fPIC -D_GNU_SOURCE" LDFLAGS="-lm" > /dev/null
        cp .build/libft8.so "$DEPS/lib/" && cp ft8/*.h "$DEPS/include/ft8lib/"
        cd "$TOP"
    fi
}

build() {
    deps
    echo "== the GUI"
    cmake -S . -B "$B" -G Ninja -DHOST_BUILD=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo > /dev/null
    cmake --build "$B" --target x6100_gui params_sqlite
    [ -e "$B/data/params.db" ] || { mkdir -p "$B/data"; cp "$B/params.db" "$B/data/params.db"; }
}

run() {
    [ -e "$B/data/params.db" ] || { mkdir -p "$B/data"; cp "$B/params.db" "$B/data/params.db"; }
    exec "$B/src/x6100_gui"
}

case "${1:-all}" in
    build) build ;;
    run)   run ;;
    clean) rm -rf "$B" ;;
    all)   build && run ;;
    *)     echo "usage: host/build.sh [build|run|clean]" >&2; exit 2 ;;
esac
