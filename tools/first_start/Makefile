# Host tests of the first-start guide: `make` builds them and runs them. first_start_kbd_test: its Keyboard step, what
# it shows from what Bluetooth and the keyboard say (src/first_start_kbd.c, #291), plain C. The others: when the guide
# comes up by itself (first_start_rule.h), for both radios. The rule (src/first_start_rule.c) and the real src/cfg
# (the settings, their pending writes, the database code) on a params.db made from sql/params.sql in a folder of its
# own, deleted as Settings > Info > Factory reset deletes it (factory_reset.h). Stand-ins for LVGL's log and async
# calls and for the GUI thread's check (stubs.cpp). See the test files for the cases. src/cfg's headers want LVGL
# beside src/ (LVGL) and ft8_lib's headers (DEPS, as host/build.sh puts them in build-host/deps).
TOP      := ../..
SRC      := $(TOP)/src
DEPS     ?= $(TOP)/build-host/deps
LVGL     ?= $(TOP)/lvgl
# A host build names the data folder (board.h): settings_manager_api.cpp keeps the own TX list's file there (#247),
# which no case here reads
DEFS     := -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DBOARD_DATA='"$(CURDIR)/build"'
INC      := -I $(TOP) -I $(SRC) -I $(SRC)/cfg -I $(DEPS)/include -isystem $(LVGL)
CFLAGS   := -std=gnu11 -O1 -g -Wall $(DEFS) $(INC)
CXXFLAGS := -std=gnu++17 -O1 -g $(DEFS) $(INC)
CFG_CPP  := $(wildcard $(SRC)/cfg/*.cpp)
CFG_C    := $(wildcard $(SRC)/cfg/*.c) $(SRC)/first_start_rule.c
HEADERS  := $(wildcard $(SRC)/cfg/*.h) $(SRC)/board.h $(SRC)/radio_types.h $(SRC)/first_start_rule.h \
            $(SRC)/factory_reset.h
TEST     := first_start_test.cpp stubs.cpp

all: test

build/x6100/%.o: $(SRC)/cfg/%.c $(HEADERS)
	mkdir -p build/x6100 && $(CC) $(CFLAGS) -c -o $@ $<

build/x6200/%.o: $(SRC)/cfg/%.c $(HEADERS)
	mkdir -p build/x6200 && $(CC) $(CFLAGS) -DBOARD_X6200=1 -c -o $@ $<

build/x6100/%.o: $(SRC)/%.c $(HEADERS)
	mkdir -p build/x6100 && $(CC) $(CFLAGS) -c -o $@ $<

build/x6200/%.o: $(SRC)/%.c $(HEADERS)
	mkdir -p build/x6200 && $(CC) $(CFLAGS) -DBOARD_X6200=1 -c -o $@ $<

OBJS = $(foreach f,$(CFG_C),build/$(1)/$(basename $(notdir $(f))).o)

first_start_x6100: $(TEST) $(CFG_CPP) $(HEADERS) $(call OBJS,x6100)
	$(CXX) $(CXXFLAGS) -o $@ $(TEST) $(CFG_CPP) $(filter build/%,$^) -lsqlite3 -lpthread

first_start_x6200: $(TEST) $(CFG_CPP) $(HEADERS) $(call OBJS,x6200)
	$(CXX) $(CXXFLAGS) -DBOARD_X6200=1 -o $@ $(TEST) $(CFG_CPP) $(filter build/%,$^) -lsqlite3 -lpthread

first_start_kbd_test: first_start_kbd_test.c $(SRC)/first_start_kbd.c $(SRC)/first_start_kbd.h $(SRC)/bt_dev.h
	$(CC) -std=gnu11 -O1 -g -Wall -Wextra -I $(SRC) -o $@ first_start_kbd_test.c $(SRC)/first_start_kbd.c

test: first_start_x6100 first_start_x6200 first_start_kbd_test
	./first_start_x6100 $(TOP)/sql/params.sql
	./first_start_x6200 $(TOP)/sql/params.sql
	mkdir -p build && ./first_start_kbd_test build/first_start_bt.conf

clean:
	rm -rf first_start_x6100 first_start_x6200 first_start_kbd_test build

.PHONY: all test clean
