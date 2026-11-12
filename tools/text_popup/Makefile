# Host test of the text window (src/text_popup.c, #220): `make` builds it and runs it. The real text_popup.c, the
# fonts and font_select.c, and the USB keyboard's key_input.c with its kbd_*.c, compiled as the PC build compiles them,
# on the PC build's LVGL and lv_drivers (LVGL_LIB, DRV_LIB, as host/build.sh leaves them in build-host/lib; xkb's
# libxkbcommon, as the PC build). What these call of the rest of the GUI (the styles, the theme's colours, the volume,
# the paddle, the screen, the keyboard layout setting) the test stands in for; a link error names what they now
# reach. LVGL's headers beside src/ (LVGL), liquid-dsp's for the GUI's headers (DEPS, as host/build.sh puts them in
# build-host/deps). See text_popup_test.c for the cases.
TOP      := ../..
SRC      := $(TOP)/src
DEPS     ?= $(TOP)/build-host/deps
LVGL     ?= $(TOP)/lvgl
LVGL_LIB ?= $(TOP)/build-host/lib/liblvgl.a
DRV_LIB  ?= $(TOP)/build-host/lib/liblv_drivers.a
DEFS     := -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DUSE_SDL=1 -DUSE_DRM=0 -DUSE_FBDEV=0 \
            -DLV_DRAW_NEON=0
INC      := -I $(TOP) -I $(SRC) -I $(SRC)/cfg -I $(DEPS)/include -isystem $(LVGL)
GC       := -ffunction-sections -fdata-sections
# The GUI's files as the PC build has them, without its warnings; the test's own file with them
GUI      := -std=gnu11 -O1 -g -pthread $(DEFS) $(INC) $(GC)
CFLAGS   := $(GUI) -Wall -Wextra -Wno-comment
# every font fonts_apply() can put in the window: the families at each size, the 1bit theme's
SIZES    := 14 16 18 20 22 24 26 28 30 32 34 36 38 40 42 44 60
FONTS    := $(foreach f,sony roboto dejavu arial rcond codec,$(foreach n,$(SIZES),fonts/$(f)_$(n))) \
            $(foreach n,12 14 16 18 20 22 24 28 32 44,fonts/pixel_$(n))
FILES    := text_popup fonts/font_select key_input kbd_hyper kbd_keymap kbd_layout kbd_rollover $(FONTS)
OBJS     := $(FILES:%=build/%.o)

all: test

build/%.o: $(SRC)/%.c $(wildcard $(SRC)/*.h)
	mkdir -p $(dir $@) && $(CC) $(GUI) -c -o $@ $<

text_popup_test: text_popup_test.c $(OBJS) $(DRV_LIB) $(LVGL_LIB)
	$(CC) $(CFLAGS) -o $@ text_popup_test.c $(OBJS) $(DRV_LIB) $(LVGL_LIB) -Wl,--gc-sections -lxkbcommon -lm -pthread

test: text_popup_test
	./text_popup_test

clean:
	rm -rf text_popup_test build

.PHONY: all test clean
