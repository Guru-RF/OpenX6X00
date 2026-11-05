# Host test of the TX meters' readings (src/tx_meters.c, #53): `make` builds it and runs it. tx_meters_test.c includes
# the real tx_meters.c whole and gives it readings as radio.c does in the radio thread: one a base packet in TX
# (tx_info_update), tx_info_clear on the way out (tx_over). The readers look in between, as TCI, CAT, the remote, the
# S-meter box, tx_player and JS8's SWR guard do. With LVGL's real headers and liquid-dsp's (DEPS, as host/build.sh puts
# them in build-host/deps), and the real lpf (src/util.cpp). The views' job is counted, not run: with it the drawing
# drops out of the link (--gc-sections), and its calls into LVGL and the rest of the GUI with it. A link error names
# what the readings now reach: a stand-in for it goes in the test.
TOP      := ../..
SRC      := $(TOP)/src
DEPS     ?= $(TOP)/build-host/deps
LVGL     ?= $(TOP)/lvgl
DEFS     := -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DUSE_SDL=1 -DUSE_DRM=0 -DUSE_FBDEV=0
INC      := -I $(TOP) -I $(SRC) -I $(SRC)/cfg -I $(DEPS)/include -isystem $(LVGL)
GC       := -ffunction-sections -fdata-sections
# tx_meters.c inside the test: its unused parameters, and the views' job no one calls here, are no warning (nor
# params/types.h's comment, as in tools/radio_wr12)
CFLAGS   := -std=gnu11 -O1 -g -Wall -Wextra -Wno-comment -Wno-unused-parameter -Wno-unused-function $(DEFS) $(INC) $(GC)
CXXFLAGS := -std=gnu++17 -O1 -g $(DEFS) $(INC) $(GC)

all: test

build/util.o: $(SRC)/util.cpp $(SRC)/util.h
	mkdir -p build && $(CXX) $(CXXFLAGS) -c -o $@ $<

tx_meters_test: tx_meters_test.c $(SRC)/tx_meters.c $(SRC)/tx_meters.h build/util.o
	$(CC) $(CFLAGS) -o $@ tx_meters_test.c build/util.o -Wl,--gc-sections -lstdc++ -lm

test: tx_meters_test
	./tx_meters_test

clean:
	rm -rf tx_meters_test build

.PHONY: all test clean
