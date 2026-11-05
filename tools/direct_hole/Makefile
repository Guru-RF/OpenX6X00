# Host test of direct mode's see-through hole (direct_hole_cb in src/waterfall_view.c, #177): `make` builds it and
# runs it. The PC build has no direct mode (USE_DRM=0), so there nothing ever adds the callback: here the real
# waterfall_view.c, compiled as the PC build compiles it, gets its draw event from the PC build's LVGL (LVGL_LIB, as
# host/build.sh leaves it in build-host/lib) on an object of the test's. Only the hole is reached: the rest of
# waterfall_view.c drops out of the link (--gc-sections), and its calls into the GUI and the DSP with it. A link
# error names what the hole now reaches. LVGL's headers beside src/ (LVGL), liquid-dsp's for dsp.h (DEPS, as
# host/build.sh puts them in build-host/deps). See direct_hole_test.c for the cases.
TOP      := ../..
SRC      := $(TOP)/src
DEPS     ?= $(TOP)/build-host/deps
LVGL     ?= $(TOP)/lvgl
LVGL_LIB ?= $(TOP)/build-host/lib/liblvgl.a
DEFS     := -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DUSE_SDL=1 -DUSE_DRM=0 -DUSE_FBDEV=0
INC      := -I $(TOP) -I $(SRC) -I $(SRC)/cfg -I $(DEPS)/include -isystem $(LVGL)
GC       := -ffunction-sections -fdata-sections
# waterfall_view.c as the PC build has it, without its warnings; the test's own file with them
GUI      := -std=gnu11 -O1 -g -pthread $(DEFS) $(INC) $(GC)
CFLAGS   := $(GUI) -Wall -Wextra -Wno-comment

all: test

build/waterfall_view.o: $(SRC)/waterfall_view.c $(wildcard $(SRC)/*.h)
	mkdir -p build && $(CC) $(GUI) -c -o $@ $(SRC)/waterfall_view.c

direct_hole_test: direct_hole_test.c build/waterfall_view.o $(LVGL_LIB)
	$(CC) $(CFLAGS) -o $@ direct_hole_test.c build/waterfall_view.o $(LVGL_LIB) -Wl,--gc-sections -lm -pthread

test: direct_hole_test
	./direct_hole_test

clean:
	rm -rf direct_hole_test build

.PHONY: all test clean
