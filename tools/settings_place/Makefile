# Host test of Settings opening where it was left (src/settings_place.c, #362): `make` builds it and runs it. The real
# settings_place.c, compiled as the PC build compiles it, on the PC build's LVGL (LVGL_LIB, as host/build.sh leaves it
# in build-host/lib); the pages are built in the test the way settings_window.cpp builds its tabs. LVGL's headers
# beside src/ (LVGL). See settings_place_test.c for the cases.
TOP      := ../..
SRC      := $(TOP)/src
DEPS     ?= $(TOP)/build-host/deps
LVGL     ?= $(TOP)/lvgl
LVGL_LIB ?= $(TOP)/build-host/lib/liblvgl.a
DEFS     := -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DLV_DRAW_NEON=0
INC      := -I $(TOP) -I $(SRC) -I $(DEPS)/include -isystem $(LVGL)
GC       := -ffunction-sections -fdata-sections
# The GUI's file as the PC build has it, without its warnings; the test's own file with them
GUI      := -std=gnu11 -O1 -g -pthread $(DEFS) $(INC) $(GC)
CFLAGS   := $(GUI) -Wall -Wextra -Wno-comment

all: test

build/settings_place.o: $(SRC)/settings_place.c $(SRC)/settings_place.h
	mkdir -p build && $(CC) $(GUI) -c -o $@ $<

settings_place_test: settings_place_test.c build/settings_place.o $(LVGL_LIB)
	$(CC) $(CFLAGS) -o $@ settings_place_test.c build/settings_place.o $(LVGL_LIB) -Wl,--gc-sections -lm -pthread

test: settings_place_test
	./settings_place_test

clean:
	rm -rf settings_place_test build

.PHONY: all test clean
