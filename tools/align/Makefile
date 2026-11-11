# Host test of align_int (src/util.cpp), what the main knob lands on: `make` builds it and runs it. With a uint16_t step
# a 100 kHz detent (#234), or 5 kHz turned fast on Strong (75 kHz), was cut to step % 65536 and landed off the grid:
# 7.020 MHz turned down one 100 kHz detent read 6.892.800 on the bench X6100 (11 Nov). The real util.cpp, with LVGL's
# and the deps' headers as tools/tx_meters builds it.
TOP      := ../..
SRC      := $(TOP)/src
DEPS     ?= $(TOP)/build-host/deps
LVGL     ?= $(TOP)/lvgl
DEFS     := -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DUSE_SDL=1 -DUSE_DRM=0 -DUSE_FBDEV=0
INC      := -I $(TOP) -I $(SRC) -I $(SRC)/cfg -I $(DEPS)/include -isystem $(LVGL)
# util.cpp's DSP helpers (liquid-dsp) drop out of the link: only align_int is called
GC       := -ffunction-sections -fdata-sections
CFLAGS   := -std=gnu11 -O1 -g -Wall -Wextra $(DEFS) $(INC) $(GC)
CXXFLAGS := -std=gnu++17 -O1 -g $(DEFS) $(INC) $(GC)

all: test

build/util.o: $(SRC)/util.cpp $(SRC)/util.h $(SRC)/common/math.h
	mkdir -p build && $(CXX) $(CXXFLAGS) -c -o $@ $<

align_test: align_test.c $(SRC)/util.h build/util.o
	$(CC) $(CFLAGS) -o $@ align_test.c build/util.o -Wl,--gc-sections -lstdc++ -lm

test: align_test
	./align_test

clean:
	rm -rf align_test build

.PHONY: all test clean
