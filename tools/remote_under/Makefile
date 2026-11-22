# Host test of the remote screen's picture with the DRM planes (drm_compose_landscape_from() in
# lv_drivers/display/drm.c): `make` builds it and runs it. The real drm.c compiled for the PC with the planes' code in
# (USE_DRM=1), linked with libdrm and the PC build's LVGL (LVGL_LIB, as host/build.sh leaves it in build-host/lib);
# no DRM device is opened, only the picture's arithmetic runs, and what drm.c does not reach of it drops out of the
# link (--gc-sections). Needs libdrm's headers (pkg-config libdrm). See remote_under_test.c for the cases. The time
# it prints is this PC's: on the radio the plane's pixels along a landscape row each miss the cache.
TOP      := ../..
SRC      := $(TOP)/src
LVGL     ?= $(TOP)/lvgl
LVGL_LIB ?= $(TOP)/build-host/lib/liblvgl.a
DEFS     := -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DUSE_DRM=1 -DUSE_FBDEV=0 -DUSE_SDL=0
INC      := -I $(TOP) -I $(SRC) -isystem $(LVGL) $(shell pkg-config --cflags libdrm)
CFLAGS   := -std=gnu11 -O2 -g $(DEFS) $(INC) -ffunction-sections -fdata-sections

all: test

build/drm.o: $(TOP)/lv_drivers/display/drm.c $(TOP)/lv_drivers/display/drm.h $(SRC)/board.h
	mkdir -p build && $(CC) $(CFLAGS) -c -o $@ $<

remote_under_test: remote_under_test.c build/drm.o $(LVGL_LIB)
	$(CC) $(CFLAGS) -Wall -Wextra -o $@ remote_under_test.c build/drm.o $(LVGL_LIB) -Wl,--gc-sections \
	    $(shell pkg-config --libs libdrm) -lm -pthread

test: remote_under_test
	./remote_under_test

clean:
	rm -rf remote_under_test build

.PHONY: all test clean
