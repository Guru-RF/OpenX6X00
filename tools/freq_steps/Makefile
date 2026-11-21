# Host tests of the tuning steps FST goes through: `make` builds them and runs them.
# - freq_steps_test: src/cfg/freq_steps.c alone. The whole list as before, Settings > Display > Tuning steps narrowed to
#   a range, a step outside it, a knob stopping at the ends. See freq_steps_test.c for the cases.
# - freq_steps_cfg_x6100 / _x6200: the real src/cfg (as tools/iambic_default builds it) on a database made in memory from
#   sql/params.sql: FST's step change and the knob's stop with the range, the range kept at the next start. src/cfg's
#   headers want LVGL beside src/ (LVGL) and ft8_lib's headers (DEPS, as host/build.sh puts them in build-host/deps).
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
HEADERS  := $(wildcard $(SRC)/cfg/*.h) $(SRC)/board.h $(SRC)/radio_types.h
TEST     := freq_steps_cfg_test.cpp stubs.cpp

all: test

freq_steps_test: freq_steps_test.c $(SRC)/cfg/freq_steps.c $(SRC)/cfg/freq_steps.h
	$(CC) -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC)/cfg -o $@ freq_steps_test.c $(SRC)/cfg/freq_steps.c

build/x6100/%.o: $(SRC)/cfg/%.c $(HEADERS)
	mkdir -p build/x6100 && $(CC) $(CFLAGS) -c -o $@ $<

build/x6200/%.o: $(SRC)/cfg/%.c $(HEADERS)
	mkdir -p build/x6200 && $(CC) $(CFLAGS) -DBOARD_X6200=1 -c -o $@ $<

freq_steps_cfg_x6100: $(TEST) $(CFG_CPP) $(HEADERS) $(patsubst $(SRC)/cfg/%.c,build/x6100/%.o,$(CFG_C))
	$(CXX) $(CXXFLAGS) -o $@ $(TEST) $(CFG_CPP) $(filter build/%,$^) -lsqlite3 -lpthread

freq_steps_cfg_x6200: $(TEST) $(CFG_CPP) $(HEADERS) $(patsubst $(SRC)/cfg/%.c,build/x6200/%.o,$(CFG_C))
	$(CXX) $(CXXFLAGS) -DBOARD_X6200=1 -o $@ $(TEST) $(CFG_CPP) $(filter build/%,$^) -lsqlite3 -lpthread

test: freq_steps_test freq_steps_cfg_x6100 freq_steps_cfg_x6200
	./freq_steps_test
	./freq_steps_cfg_x6100 $(TOP)/sql/params.sql
	./freq_steps_cfg_x6200 $(TOP)/sql/params.sql

clean:
	rm -rf freq_steps_test freq_steps_cfg_x6100 freq_steps_cfg_x6200 build

.PHONY: all test clean
