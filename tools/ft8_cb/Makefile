# Host test of CB callsigns in the FT8/FT4 app (#286): `make` builds and runs it, `make verbose` prints every case.
# The messages of a QSO between two CB calls as WSJT-CB puts them on the air, through the app's encoder
# (src/ft8/msg_encode.c) and a packer of the 77-bit format written in the test: the same bits; decoded by ft8_lib
# with a station's hash table; and a QSO between two radios with the app's sequence (src/ft8/qso.cpp), Auto on, over
# FT8 and FT4 audio made and decoded by the app's worker (src/ft8/worker.c); no country for a CB call (src/dxcc.c with
# third-party/cty/cty.dat). See ft8_cb_test.c.
# The decoder's sources are the GUI's, built with their own warnings left off; ft8_lib's headers and libraries are
# host/build.sh's (DEPS, build-host/deps by default). ../ft8_sync/stub_include stands in for LVGL's log.
TOP    := ../..
SRC    := $(TOP)/src
DEPS   ?= $(TOP)/build-host/deps
DATA   := $(CURDIR)/build/data
INC    := -I ../ft8_sync/stub_include -I $(SRC) -I $(SRC)/ft8 -I $(DEPS)/include
CTY    := $(abspath $(TOP)/third-party/cty/cty.dat)
CFLAGS := -std=gnu11 -O1 -g -pthread -DHOST_BUILD=1 -DBOARD_DATA='"$(DATA)"' -DCTY_SD='"$(CTY)"' \
          -DCTY_IMAGE='"$(CTY)"' $(INC)
WARN   := -Wall -Wextra
LIBS   := -L $(DEPS)/lib -lft8 -lliquid -lm -lstdc++ -Wl,-rpath,$(abspath $(DEPS)/lib)
OURS   := cb.c dxcc.c ft8/msg_encode.c ft8/utils.c qth/qth.c
GUI    := ft8/worker.c ft8/gfsk.c ft8/hound.c
HDRS   := $(wildcard $(SRC)/ft8/*.h) $(SRC)/qth/qth.h $(SRC)/cb.h $(SRC)/dxcc.h

all: test

build/%.o: $(SRC)/%.c $(HDRS)
	mkdir -p $(dir $@) && $(CC) $(CFLAGS) $(if $(filter $(OURS),$*.c),$(WARN)) -c -o $@ $<

build/ft8/qso.o: $(SRC)/ft8/qso.cpp $(HDRS)
	mkdir -p $(dir $@) && $(CXX) -std=c++17 -O1 -g $(INC) -c -o $@ $<

ft8_cb_test: ft8_cb_test.c $(patsubst %.c,build/%.o,$(OURS) $(GUI)) build/ft8/qso.o
	$(CC) $(CFLAGS) $(WARN) -o $@ $^ $(LIBS)

test: ft8_cb_test
	mkdir -p $(DATA)
	./ft8_cb_test

verbose: ft8_cb_test
	mkdir -p $(DATA)
	./ft8_cb_test -v

clean:
	rm -rf ft8_cb_test build

.PHONY: all test verbose clean
