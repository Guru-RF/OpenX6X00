# Host test of the CB and HF Air Band apps' tables (src/cb.c, src/aero.c): `make` builds both parts and runs them.
# aerocb_test: each CB plan's channel to frequency and its modes, the knob's channel steps, the HF aero families
# against the ITU Plan's channel raster, the areas (aerocb_test.c). aerocb_tx_*: every frequency of both apps through
# the real cfg_tx_refusal on the real settings (src/cfg on a database made in memory from sql/params.sql, as
# tools/iambic_default does): refused with TX outside ham bands off, as anywhere outside the amateur bands; allowed
# with it on; the TX ranges narrower still (aerocb_tx_test.cpp). src/cfg's headers want LVGL beside src/ (LVGL) and
# ft8_lib's headers (DEPS, as host/build.sh puts them in build-host/deps).
TOP      := ../..
SRC      := $(TOP)/src
DEPS     ?= $(TOP)/build-host/deps
LVGL     ?= $(TOP)/lvgl
# A host build names the data folder (board.h): settings_manager_api.cpp keeps the own TX list's file there, which
# the test does not read (it sets the list from text)
DEFS     := -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DBOARD_DATA='"$(CURDIR)/build"'
INC      := -I $(TOP) -I $(SRC) -I $(SRC)/cfg -I $(DEPS)/include -isystem $(LVGL)
CFLAGS   := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC)
CFG_CF   := -std=gnu11 -O1 -g $(DEFS) $(INC)
CXXFLAGS := -std=gnu++17 -O1 -g $(DEFS) $(INC)
CFG_CPP  := $(wildcard $(SRC)/cfg/*.cpp)
CFG_C    := $(wildcard $(SRC)/cfg/*.c)
HEADERS  := $(wildcard $(SRC)/cfg/*.h) $(SRC)/board.h $(SRC)/radio_types.h $(SRC)/cb.h $(SRC)/aero.h
TABLES   := $(SRC)/cb.c $(SRC)/aero.c
TX_TEST  := aerocb_tx_test.cpp stubs.cpp

all: test

aerocb_test: aerocb_test.c $(TABLES) $(SRC)/cb.h $(SRC)/aero.h $(SRC)/cfg/tx_ranges.c $(SRC)/cfg/tx_ranges.h \
             $(SRC)/cfg/ham_bands.c $(SRC)/cfg/ham_bands.h
	$(CC) $(CFLAGS) -o $@ aerocb_test.c $(TABLES) $(SRC)/cfg/tx_ranges.c $(SRC)/cfg/ham_bands.c -pthread

build/x6100/%.o: $(SRC)/cfg/%.c $(HEADERS)
	mkdir -p build/x6100 && $(CC) $(CFG_CF) -c -o $@ $<

build/x6200/%.o: $(SRC)/cfg/%.c $(HEADERS)
	mkdir -p build/x6200 && $(CC) $(CFG_CF) -DBOARD_X6200=1 -c -o $@ $<

build/%.o: $(SRC)/%.c $(HEADERS)
	mkdir -p build && $(CC) $(CFG_CF) -c -o $@ $<

aerocb_tx_x6100: $(TX_TEST) $(CFG_CPP) $(HEADERS) $(patsubst $(SRC)/cfg/%.c,build/x6100/%.o,$(CFG_C)) build/cb.o \
                 build/aero.o
	$(CXX) $(CXXFLAGS) -o $@ $(TX_TEST) $(CFG_CPP) $(filter build/%,$^) -lsqlite3 -lpthread

aerocb_tx_x6200: $(TX_TEST) $(CFG_CPP) $(HEADERS) $(patsubst $(SRC)/cfg/%.c,build/x6200/%.o,$(CFG_C)) build/cb.o \
                 build/aero.o
	$(CXX) $(CXXFLAGS) -DBOARD_X6200=1 -o $@ $(TX_TEST) $(CFG_CPP) $(filter build/%,$^) -lsqlite3 -lpthread

test: aerocb_test aerocb_tx_x6100 aerocb_tx_x6200
	./aerocb_test
	./aerocb_tx_x6100 $(TOP)/sql/params.sql
	./aerocb_tx_x6200 $(TOP)/sql/params.sql

clean:
	rm -rf aerocb_test aerocb_tx_x6100 aerocb_tx_x6200 build

.PHONY: all test clean
