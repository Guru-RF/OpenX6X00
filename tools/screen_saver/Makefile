# Host test of the screensaver's picture and of screen_light.c's stages (src/screen_saver.c, screen_light.c,
# screen_idle.c, mono.c and fonts/matrix_20.c, compiled as the PC build compiles them): `make` builds it and runs it.
# The PC build's LVGL (LVGL_LIB, as host/build.sh leaves it in build-host/lib) draws the rain both ways, the PC's and
# the radios' transparent screen, which the PC build never runs. What screen_light.c calls of the rest of the GUI
# (the radio's state, the params, the recorder, the waterfall and the spectrum) the test stands in for; a link error
# names what it now reaches. LVGL's headers beside src/ (LVGL), liquid-dsp's and ft8lib's for the GUI's headers (DEPS,
# as host/build.sh puts them in build-host/deps). See screen_saver_test.c for the cases.
TOP      := ../..
SRC      := $(TOP)/src
DEPS     ?= $(TOP)/build-host/deps
LVGL     ?= $(TOP)/lvgl
LVGL_LIB ?= $(TOP)/build-host/lib/liblvgl.a
DEFS     := -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DUSE_SDL=1 -DUSE_DRM=0 -DUSE_FBDEV=0 \
            -DLV_DRAW_NEON=0
INC      := -I $(TOP) -I $(SRC) -I $(SRC)/cfg -I $(DEPS)/include -isystem $(LVGL)
GC       := -ffunction-sections -fdata-sections
# The GUI's files as the PC build has them, without its warnings; the test's own file with them
GUI      := -std=gnu11 -O1 -g -pthread $(DEFS) $(INC) $(GC)
CFLAGS   := $(GUI) -Wall -Wextra -Wno-comment
FILES    := screen_saver screen_light screen_idle mono fonts/matrix_20
OBJS     := $(FILES:%=build/%.o)

all: test

build/%.o: $(SRC)/%.c $(wildcard $(SRC)/*.h)
	mkdir -p $(dir $@) && $(CC) $(GUI) -c -o $@ $<

screen_saver_test: screen_saver_test.c $(OBJS) $(LVGL_LIB)
	$(CC) $(CFLAGS) -o $@ screen_saver_test.c $(OBJS) $(LVGL_LIB) -Wl,--gc-sections -lm -pthread

test: screen_saver_test
	./screen_saver_test

clean:
	rm -rf screen_saver_test build

.PHONY: all test clean
