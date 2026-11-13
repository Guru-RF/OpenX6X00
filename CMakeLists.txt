# Xiegu X6100 LVGL GUI - top level build
#
# Copyright (c) 2026 Giovanni Ricciardi IU1WXY
#
# Two ways in: the radio build (buildroot toolchain, Cortex-A7 with NEON) and
# the PC test build (cmake -DENABLE_TESTING=YES, see run_tests.sh).

cmake_minimum_required(VERSION 3.23)

# The GUI on this PC: an SDL window for the panel, the stub radio behind hw.h, the data in a
# directory. host/build.sh configures, builds and runs it; see host/README.md
option(HOST_BUILD "Build the GUI for this PC (SDL window, stub radio)" OFF)
set(HOST_DATA_DIR "${CMAKE_BINARY_DIR}/data" CACHE PATH "The host build's data directory (the radio's /mnt)")

# All C code is C11
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

project(x6100_gui)

# Headers are included from the top: lv_conf.h, lvgl/, src/...
include_directories(.)

if(ENABLE_TESTING)
        # Tests run on the PC: Catch2 and the radio control library are
        # fetched. Everything from here on, the control library included,
        # is built with LVGL drawing without NEON.
        include(FetchContent)
        FetchContent_Declare(Catch2
                GIT_REPOSITORY https://github.com/catchorg/Catch2.git
                GIT_TAG        v3.4.0)
        FetchContent_MakeAvailable(Catch2)
        add_compile_definitions(LV_DRAW_NEON=0)
        FetchContent_Declare(aether_x6100_control
                GIT_REPOSITORY https://github.com/gdyuldin/X6100Control.git
                GIT_TAG        v0.16.1)
        FetchContent_MakeAvailable(aether_x6100_control)
elseif(HOST_BUILD)
        # This PC: no NEON, the SDL window driver, the host board (board.h), the data and share
        # directories under the build directory; liquid-dsp and ft8_lib from HOST_DEPS, build-host/deps
        # by default (host/build.sh puts them in its build folder's deps/ and passes that)
        add_compile_definitions(LV_DRAW_NEON=0 HOST_BUILD=1 USE_SDL=1 USE_DRM=0 USE_FBDEV=0)
        add_compile_definitions(BOARD_DATA="${HOST_DATA_DIR}" BOARD_SHARE="${CMAKE_BINARY_DIR}/share")
        add_compile_definitions(X6100_SBIN="${CMAKE_SOURCE_DIR}/rootfs/usr/sbin")    # the helpers, from the tree
        set(LV_DRIVERS_SDL ON)
        set(HW_BACKEND "stub" CACHE STRING "The radio backend: x6100 or stub")
        set(HOST_DEPS "${CMAKE_SOURCE_DIR}/build-host/deps" CACHE PATH "The host build's liquid-dsp, ft8_lib and boost")
        include_directories(${HOST_DEPS}/include)
        link_directories(${HOST_DEPS}/lib)
        add_link_options(-Wl,-rpath,${HOST_DEPS}/lib)
        # What the image ships read-only, staged as the host's share directory
        file(COPY rootfs/usr/share/x6100/ DESTINATION ${CMAKE_BINARY_DIR}/share)
        file(COPY images DESTINATION ${CMAKE_BINARY_DIR}/share)
        file(COPY third-party/cty/cty.dat DESTINATION ${CMAKE_BINARY_DIR}/share)
else()
        # Allwinner A33: Cortex-A7, hard float, NEON
        add_compile_options(-mtune=cortex-a7 -mcpu=cortex-a7 -mfloat-abi=hard -mfpu=neon-vfpv4)
        # Unused functions and data are not linked
        add_compile_options(-ffunction-sections -fdata-sections)
        add_link_options(-Wl,--gc-sections)
endif()

# The radio behind src/hw/hw.h: the X6100 or the X6200 through its control library, or the
# stub (a radio made of nothing, for the GUI on a desktop and as the template for a new radio's
# backend). The host build above chose the stub already
set(HW_BACKEND "x6100" CACHE STRING "The radio backend: x6100, x6200 or stub")
if(HW_BACKEND STREQUAL "x6200")
        add_compile_definitions(BOARD_X6200=1)
endif()

add_subdirectory(lvgl)

if(ENABLE_TESTING AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        # tests/ lives in the private dev repository
        enable_testing()
        add_subdirectory(tests)
else()
        add_subdirectory(third-party/js8core)     # JS8Call's modem, for src/js8
        add_subdirectory(third-party/wsprd)
        add_subdirectory(src)
        add_subdirectory(lv_drivers)
        add_subdirectory(sql)
        if(NOT HOST_BUILD)
                add_subdirectory(btcat)       # the radio's own programs
                add_subdirectory(bootanim)
        endif()
        # The Winlink modems for Pat: ardopcf, and Mercury (experimental). On the PC too, where they are
        # built only when asked for by name, for the Winlink tests (tools/winlink)
        add_subdirectory(third-party/ardopcf)
        add_subdirectory(third-party/mercury)

        # Installed into the buildroot target: the GUI, the rootfs overlay
        # (init scripts, tools, data) and the images
        install(TARGETS ${PROJECT_NAME} DESTINATION sbin)
        # NetworkManager's profiles root's own and 0600, or it leaves them out: git keeps 0644 (the update bundle and
        # the card image take the mode of the installed file, tools/update/mkbundle.py; both make it root's)
        install(DIRECTORY rootfs/ DESTINATION / USE_SOURCE_PERMISSIONS
                PATTERN "*.nmconnection" PERMISSIONS OWNER_READ OWNER_WRITE)
        install(DIRECTORY images DESTINATION share/x6100)
        # Callsign -> country for the FT8/FT4 table (AD1C, country-files.com)
        install(FILES third-party/cty/cty.dat DESTINATION share/x6100)
        # No folder theme is installed: themes/ has them to copy to /mnt/themes (themes/README.md); themes/RS is
        # cut from a screenshot of another maker's remote-control program (themes/make_rs.py, personal use only)
        # Pat, the Winlink client: built outside, by tools/pat/build.sh (buildroot/local-build.sh
        # passes its path), and installed as /usr/bin/pat, so the update bundle carries it too
        set(PAT_BINARY "" CACHE FILEPATH "Pat for the radio (tools/pat/build.sh)")
        if(PAT_BINARY)
                install(PROGRAMS ${PAT_BINARY} DESTINATION bin RENAME pat)
        endif()
        # XARA, the VARA-compatible Winlink modem (experimental): built outside too, by tools/xara/build.sh with
        # its own pinned Rust, and installed as /usr/bin/xara. Its licence and the notices of what is compiled
        # into it (the doc/ beside the binary) go to /usr/share/x6100/licenses/xara: buildroot drops
        # /usr/share/doc from the image
        set(XARA_BINARY "" CACHE FILEPATH "XARA for the radio (tools/xara/build.sh)")
        if(XARA_BINARY)
                get_filename_component(XARA_DIR ${XARA_BINARY} DIRECTORY)
                install(PROGRAMS ${XARA_BINARY} DESTINATION bin RENAME xara)
                install(FILES ${XARA_DIR}/doc/LICENSE ${XARA_DIR}/doc/THIRD-PARTY
                        ${XARA_DIR}/doc/rust-std-COPYRIGHT.html.gz DESTINATION share/x6100/licenses/xara)
        endif()
        # RADE, the RADE app's modem: rade_c (RADE V1) and the Opus fork with FARGAN, built outside by
        # tools/rade/build.sh and linked into the GUI (src/CMakeLists.txt). Their licences, rade_c's (BSD-2-Clause),
        # Opus's and kiss_fft's in rade_c (BSD-3-Clause), from the prefix's doc/ to /usr/share/x6100/licenses/rade
        if(RADE_PREFIX)
                install(FILES ${RADE_PREFIX}/doc/rade_c-LICENSE ${RADE_PREFIX}/doc/opus-COPYING
                        ${RADE_PREFIX}/doc/kiss_fft-COPYING
                        DESTINATION share/x6100/licenses/rade)
        endif()
        # The apps' version, for the web configurator and the scripts. Read at configure time, so a
        # changed main.h must configure again: a plain rebuild left the old number in the file (and
        # in the update bundle, whose radios then thought they ran 0.1.0)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/main.h)
        file(STRINGS src/main.h APP_VERSION REGEX "define VERSION ")
        string(REGEX REPLACE ".* v([0-9.]+).*" "\\1" APP_VERSION "${APP_VERSION}")
        file(WRITE ${CMAKE_BINARY_DIR}/app-version "${APP_VERSION}\n")
        install(FILES ${CMAKE_BINARY_DIR}/app-version DESTINATION share/x6100)
endif()
