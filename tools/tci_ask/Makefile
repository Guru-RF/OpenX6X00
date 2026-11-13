# Host test of the radio's question before a TCI client on the network transmits (src/tci_ask.c): `make` builds it
# and runs it. The real tci_ask.c and the stock fonts it draws with, compiled as the PC build compiles them, on the PC
# build's LVGL (LVGL_LIB, as host/build.sh leaves it in build-host/lib). What tci_ask.c calls of the rest of the GUI
# (the styles, the theme's colours, the volume, the screen light, the notes, the TCI server's answer) the test stands
# in for; a link error names what it now reaches. LVGL's headers beside src/ (LVGL), liquid-dsp's for the GUI's
# headers (DEPS, as host/build.sh puts them in build-host/deps). See tci_ask_test.c for the cases.
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
FILES    := tci_ask fonts/sony_16 fonts/sony_18 fonts/sony_22 fonts/sony_26
OBJS     := $(FILES:%=build/%.o)

all: test

build/%.o: $(SRC)/%.c $(wildcard $(SRC)/*.h)
	mkdir -p $(dir $@) && $(CC) $(GUI) -c -o $@ $<

tci_ask_test: tci_ask_test.c $(OBJS) $(LVGL_LIB)
	$(CC) $(CFLAGS) -o $@ tci_ask_test.c $(OBJS) $(LVGL_LIB) -Wl,--gc-sections -lm -pthread

test: tci_ask_test
	./tci_ask_test

clean:
	rm -rf tci_ask_test build

.PHONY: all test clean
