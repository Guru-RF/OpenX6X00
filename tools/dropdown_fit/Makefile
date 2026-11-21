# Host test of a long dropdown list (src/dropdown_fit.c, #310): `make` builds it and runs it. The real dropdown_fit.c,
# the countries (src/countries.c and its list) and the fonts with font_select.c, compiled as the PC build compiles
# them, on the PC build's LVGL (LVGL_LIB, as host/build.sh leaves it in build-host/lib). What countries.c calls of the
# rest of the GUI (the language, the setting, the band plan, cty.dat) the test stands in for. LVGL's headers beside
# src/ (LVGL). See dropdown_fit_test.c for the cases.
TOP      := ../..
SRC      := $(TOP)/src
DEPS     ?= $(TOP)/build-host/deps
LVGL     ?= $(TOP)/lvgl
LVGL_LIB ?= $(TOP)/build-host/lib/liblvgl.a
DEFS     := -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DLV_DRAW_NEON=0
INC      := -I $(TOP) -I $(SRC) -I $(SRC)/cfg -I $(DEPS)/include -isystem $(LVGL)
GC       := -ffunction-sections -fdata-sections
# The GUI's files as the PC build has them, without its warnings; the test's own file with them
GUI      := -std=gnu11 -O1 -g -pthread $(DEFS) $(INC) $(GC)
CFLAGS   := $(GUI) -Wall -Wextra -Wno-comment
# every font fonts_apply() can put in a list: the families at each size, the letters behind them (latin1), the 1bit
# theme's
SIZES    := 14 16 18 20 22 24 26 28 30 32 34 36 38 40 42 44 60
FONTS    := $(foreach f,sony roboto dejavu arial rcond codec latin1,$(foreach n,$(SIZES),fonts/$(f)_$(n))) \
            $(foreach n,12 14 16 18 20 22 24 28 32 44,fonts/pixel_$(n))
FILES    := dropdown_fit countries fonts/font_select $(FONTS)
OBJS     := $(FILES:%=build/%.o)

all: test

build/%.o: $(SRC)/%.c $(wildcard $(SRC)/*.h) $(SRC)/countries_data.inc
	mkdir -p $(dir $@) && $(CC) $(GUI) -c -o $@ $<

dropdown_fit_test: dropdown_fit_test.c $(OBJS) $(LVGL_LIB)
	$(CC) $(CFLAGS) -o $@ dropdown_fit_test.c $(OBJS) $(LVGL_LIB) -Wl,--gc-sections -lm -pthread

test: dropdown_fit_test
	./dropdown_fit_test

clean:
	rm -rf dropdown_fit_test build

.PHONY: all test clean
