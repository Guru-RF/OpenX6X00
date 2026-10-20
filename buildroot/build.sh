#!/bin/bash
#
# Cross-builds the GUI outside the buildroot tree, into buildroot/build, with
# the toolchain and libraries of a buildroot already built (path in conf.sh).
# Quicker than a package rebuild when chasing a compile error.
#
# Copyright (c) 2026 Giovanni Ricciardi IU1WXY
#

source ./conf.sh

PATH=../${BUILDROOT}/host/bin:${PATH}

mkdir build
cd build

# Same settings buildroot passes to a cmake package; no docs, examples or
# tests, static libraries, LVGL without its examples and demos.
OPTS=(
    -DCMAKE_BUILD_TYPE:STRING=Release
    -DCMAKE_TOOLCHAIN_FILE=../${BUILDROOT}/host/share/buildroot/toolchainfile.cmake
    -DCMAKE_INSTALL_PREFIX="/usr"
    -DCMAKE_INSTALL_RUNSTATEDIR="/run"
    -DCMAKE_COLOR_MAKEFILE=OFF
)
for o in BUILD_DOC BUILD_DOCS BUILD_EXAMPLE BUILD_EXAMPLES BUILD_TEST BUILD_TESTS BUILD_TESTING \
         BUILD_SHARED_LIBS BUILD_SHARED_LIB; do
    OPTS+=(-D$o=OFF)
done
OPTS+=(-DLV_CONF_BUILD_DISABLE_EXAMPLES=ON -DLV_CONF_BUILD_DISABLE_DEMOS=ON)

cmake ../../ "${OPTS[@]}"

make -j$(nproc)

cd -
