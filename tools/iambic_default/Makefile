# Host test of KEY Iambic's default (#109): `make` builds it for both radios and runs it. The real src/cfg (the
# settings, their pending writes, the database code) on a database made in memory from sql/params.sql, with stand-ins
# for LVGL's log and async calls and for the GUI thread's check (stubs.cpp). See iambic_default_test.cpp for the
# cases. src/cfg's headers want LVGL beside src/ (LVGL) and ft8_lib's headers (DEPS, as host/build.sh puts them in
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
HEADERS  := $(wildcard $(SRC)/cfg/*.h) $(SRC)/board.h $(SRC)/radio_types.h
TEST     := iambic_default_test.cpp stubs.cpp

all: test

build/x6100/%.o: $(SRC)/cfg/%.c $(HEADERS)
	mkdir -p build/x6100 && $(CC) $(CFLAGS) -c -o $@ $<

build/x6200/%.o: $(SRC)/cfg/%.c $(HEADERS)
	mkdir -p build/x6200 && $(CC) $(CFLAGS) -DBOARD_X6200=1 -c -o $@ $<

iambic_default_x6100: $(TEST) $(CFG_CPP) $(HEADERS) $(patsubst $(SRC)/cfg/%.c,build/x6100/%.o,$(CFG_C))
	$(CXX) $(CXXFLAGS) -o $@ $(TEST) $(CFG_CPP) $(filter build/%,$^) -lsqlite3 -lpthread

iambic_default_x6200: $(TEST) $(CFG_CPP) $(HEADERS) $(patsubst $(SRC)/cfg/%.c,build/x6200/%.o,$(CFG_C))
	$(CXX) $(CXXFLAGS) -DBOARD_X6200=1 -o $@ $(TEST) $(CFG_CPP) $(filter build/%,$^) -lsqlite3 -lpthread

test: iambic_default_x6100 iambic_default_x6200
	./iambic_default_x6100 $(TOP)/sql/params.sql
	./iambic_default_x6200 $(TOP)/sql/params.sql

clean:
	rm -rf iambic_default_x6100 iambic_default_x6200 build

.PHONY: all test clean
