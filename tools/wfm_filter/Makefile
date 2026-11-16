# Host tests of the X6200's wide FM filter as a width (#283): `make` builds them and runs them.
# - wfm_filter_test: src/wfm_filter.h alone, against the X6200 base's way with its rx filter register (1.0.8: the two
#   edges one width, nothing cut below, 16 kHz at most).
# - wfm_settings_test: the real src/cfg (the settings, their pending writes, the database code) on a database made in
#   memory from sql/params.sql, with stand-ins for LVGL's log and async calls and the GUI thread's check (stubs.cpp):
#   WFM's width and edge as the knob, the button and the Panel set them, NFM, AM and SAM as before.
# src/cfg's headers want LVGL beside src/ (LVGL) and ft8_lib's headers (DEPS, as host/build.sh puts them in
# build-host/deps). See the two tests for the cases.
TOP      := ../..
SRC      := $(TOP)/src
DEPS     ?= $(TOP)/build-host/deps
LVGL     ?= $(TOP)/lvgl
# A host build names the data folder (board.h): settings_manager_api.cpp keeps the own TX list's file there (#247),
# which no case here reads
DEFS     := -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DBOARD_DATA='"$(CURDIR)/build"' -DBOARD_X6200=1
INC      := -I $(TOP) -I $(SRC) -I $(SRC)/cfg -I $(DEPS)/include -isystem $(LVGL)
CFLAGS   := -std=gnu11 -O1 -g -Wall -Wextra -I $(SRC)
CFG_CFLAGS := -std=gnu11 -O1 -g $(DEFS) $(INC)
CXXFLAGS := -std=gnu++17 -O1 -g $(DEFS) $(INC)
CFG_CPP  := $(wildcard $(SRC)/cfg/*.cpp)
CFG_C    := $(wildcard $(SRC)/cfg/*.c)
HEADERS  := $(wildcard $(SRC)/cfg/*.h) $(SRC)/board.h $(SRC)/radio_types.h $(SRC)/wfm_filter.h

all: test

wfm_filter_test: wfm_filter_test.c $(SRC)/wfm_filter.h
	$(CC) $(CFLAGS) -o $@ wfm_filter_test.c

build/%.o: $(SRC)/cfg/%.c $(HEADERS)
	mkdir -p build && $(CC) $(CFG_CFLAGS) -c -o $@ $<

wfm_settings_test: wfm_settings_test.cpp stubs.cpp $(CFG_CPP) $(HEADERS) $(patsubst $(SRC)/cfg/%.c,build/%.o,$(CFG_C))
	$(CXX) $(CXXFLAGS) -o $@ wfm_settings_test.cpp stubs.cpp $(CFG_CPP) $(filter build/%,$^) -lsqlite3 -lpthread

test: wfm_filter_test wfm_settings_test
	./wfm_filter_test
	./wfm_settings_test $(TOP)/sql/params.sql

clean:
	rm -rf wfm_filter_test wfm_settings_test build

.PHONY: all test clean
