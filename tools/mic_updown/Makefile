# Host tests of the hand mic's UP/DOWN keys (#300): `make` builds them and runs them. mic_updown_test: what a press
# does with Channels or Tune (src/mic_updown.h, the header alone, as screen_main.c takes it), plain C.
# mic_updown_param_test: the Settings choice, Channels when never chosen, on the real src/cfg (the settings, their
# pending writes, the database code) on a database made in memory from sql/params.sql, with stand-ins for LVGL's log
# and async calls and for the GUI thread's check (stubs.cpp). The setting is the same on both radios: the X6100's
# build. src/cfg's headers want LVGL beside src/ (LVGL) and ft8_lib's headers (DEPS, as host/build.sh puts them in
# build-host/deps).
TOP      := ../..
SRC      := $(TOP)/src
DEPS     ?= $(TOP)/build-host/deps
LVGL     ?= $(TOP)/lvgl
# A host build names the data folder (board.h): settings_manager_api.cpp keeps the own TX list's file there (#247),
# which no case here reads
DEFS     := -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DBOARD_DATA='"$(CURDIR)/build"'
INC      := -I $(TOP) -I $(SRC) -I $(SRC)/cfg -I $(DEPS)/include -isystem $(LVGL)
CFLAGS   := -std=gnu11 -O1 -g $(DEFS) $(INC)
CXXFLAGS := -std=gnu++17 -O1 -g $(DEFS) $(INC)
CFG_CPP  := $(wildcard $(SRC)/cfg/*.cpp)
CFG_C    := $(wildcard $(SRC)/cfg/*.c)
HEADERS  := $(wildcard $(SRC)/cfg/*.h) $(SRC)/board.h $(SRC)/radio_types.h $(SRC)/mic_updown.h
TEST     := mic_updown_param_test.cpp stubs.cpp

all: test

mic_updown_test: mic_updown_test.c $(SRC)/mic_updown.h
	$(CC) -std=gnu11 -O1 -g -Wall -Wextra -I $(SRC) -o $@ mic_updown_test.c

build/%.o: $(SRC)/cfg/%.c $(HEADERS)
	mkdir -p build && $(CC) $(CFLAGS) -c -o $@ $<

mic_updown_param_test: $(TEST) $(CFG_CPP) $(HEADERS) $(patsubst $(SRC)/cfg/%.c,build/%.o,$(CFG_C))
	$(CXX) $(CXXFLAGS) -o $@ $(TEST) $(CFG_CPP) $(filter build/%,$^) -lsqlite3 -lpthread

test: mic_updown_test mic_updown_param_test
	./mic_updown_test
	./mic_updown_param_test $(TOP)/sql/params.sql

clean:
	rm -rf mic_updown_test mic_updown_param_test build

.PHONY: all test clean
