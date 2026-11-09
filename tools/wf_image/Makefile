# Host test of the waterfall without planes (src/waterfall_view.c's image path: the PC build, fbdev, a single DRM
# plane, #206): `make` builds it and runs it. wf_image_test.c includes the real waterfall_view.c whole and runs it on
# the PC build's LVGL (LVGL_LIB, as host/build.sh leaves it in build-host/lib), its rows through waterfall_data() as
# the DSP thread gives them, with liquid-dsp's vector maths (DEPS, as host/build.sh puts it in build-host/deps). What
# the waterfall reaches of the rest of the GUI (the parameters and their observers, the scheduler, the theme, the
# frequency scale) the test stands in for; the rest of waterfall_view.c drops out of the link (--gc-sections). A link
# error names what it now reaches: a stand-in for it goes in the test. See wf_image_test.c for the cases.
TOP      := ../..
SRC      := $(TOP)/src
DEPS     ?= $(TOP)/build-host/deps
LVGL     ?= $(TOP)/lvgl
LVGL_LIB ?= $(TOP)/build-host/lib/liblvgl.a
DEFS     := -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DUSE_SDL=1 -DUSE_DRM=0 -DUSE_FBDEV=0
INC      := -I $(TOP) -I $(SRC) -I $(SRC)/cfg -I $(DEPS)/include -isystem $(LVGL)
GC       := -ffunction-sections -fdata-sections
# waterfall_view.c inside the test: its unused parameters, the functions no case reaches and its signed and unsigned
# compared are no warning, as the PC build gives none for them (nor params/types.h's comment, as in tools/radio_wr12)
CFLAGS   := -std=gnu11 -O1 -g -pthread -Wall -Wextra -Wno-comment -Wno-unused-parameter -Wno-unused-function \
            -Wno-sign-compare \
            $(DEFS) $(INC) $(GC)

all: test

wf_image_test: wf_image_test.c $(SRC)/waterfall_view.c $(wildcard $(SRC)/*.h) $(LVGL_LIB)
	$(CC) $(CFLAGS) -o $@ wf_image_test.c $(LVGL_LIB) $(DEPS)/lib/libliquid.a -Wl,--gc-sections -lm -pthread

test: wf_image_test
	./wf_image_test

clean:
	rm -f wf_image_test

.PHONY: all test clean
