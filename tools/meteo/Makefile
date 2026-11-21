# Host test of the Weather app: `make` runs them all.
# - meteo_c_test: src/meteo.c (where the weather is asked for, Weather from and a typed place, the helper's arguments,
#   the levels, the times, a warning on one line, Weather units with the cases of units_cases.txt) with
#   station_position and station_locator_position cut out of src/station.c into build/, as on the radio, and
#   stand-ins for the GPS, the locator, the settings, the country and the language (meteo_c_test.c). meteo.h's
#   headers want LVGL beside src/.
# - meteo_alerts_test: APP > Weather > Notices (src/meteo_alerts.c) with src/meteo.c, the clock its own (--wrap=time),
#   the GUI's headers stand-ins (stub_include/: the source is copied to build/ first, as a quoted #include looks beside
#   the including file before the -I paths) and the helper a stand-in script in build/sbin that writes the test's
#   answer; its files in build/data.
# - meteo_test.py: the helper (rootfs/usr/sbin/x6100-meteo) with the services' answers saved in samples/, its network
#   a stand-in: nothing goes to any service. BOTTLE: a folder with bottle.py, by default the radio's own from a
#   buildroot tree built next to this repository, as in tools/flights; without it the web page's part is skipped.
# - meteo_page_test.js: the web Weather page's own script under Node, when there is one: Weather units on the same
#   cases of units_cases.txt as the C test, the page drawn in both units, its Units choice.
TOP    := ../..
SRC    := $(TOP)/src
LVGL   ?= $(TOP)/lvgl
BOTTLE ?= $(patsubst %/,%,$(dir $(firstword $(wildcard \
              $(TOP)/../AetherX6200Buildroot/build/build/python-bottle-*/bottle.py \
              $(TOP)/../AetherX6100Buildroot/build/build/python-bottle-*/bottle.py))))
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE \
          -I $(TOP) -I $(SRC) -isystem $(LVGL)
DATA   := $(CURDIR)/build/data
NFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -pthread -I stub_include -I build -I $(SRC) -DBOARD_DATA='"$(DATA)"' \
          -DX6100_SBIN='"$(CURDIR)/build/sbin"' -DMETEO_ALERTS_FILE='"$(DATA)/alerts.json"'

all: test

build/station_position.c: $(SRC)/station.c
	mkdir -p build
	awk '/^#define (POS_MAX_AGE_S|PHONE_MAX_ACC_M)/; /^bool station_(locator_)?position\(/,/^}/' $< > $@
	grep -q '^bool station_position(' $@ || { rm -f $@; echo "station_position not found in $<"; exit 1; }
	grep -q '^bool station_locator_position(' $@ || { rm -f $@; echo "station_locator_position not found in $<"; exit 1; }

meteo_c_test: meteo_c_test.c build/station_position.c $(SRC)/meteo.c $(SRC)/meteo.h $(SRC)/cJSON.c $(SRC)/qth/qth.c
	$(CC) $(CFLAGS) -o $@ meteo_c_test.c $(SRC)/meteo.c $(SRC)/cJSON.c $(SRC)/qth/qth.c -lm

build/.copied: $(SRC)/meteo_alerts.c $(SRC)/meteo_alerts.h
	mkdir -p build && cp $^ build/ && touch $@

# The helper's stand-in: its arguments into $$METEO_TEST_RUNS, the test's answer into the file named last
build/sbin/x6100-meteo:
	mkdir -p build/sbin
	printf '%s\n' '#!/bin/sh' 'echo "$$*" >> "$$METEO_TEST_RUNS"' \
	    '[ -n "$$METEO_TEST_FAIL" ] && { echo "error no network"; exit 0; }' \
	    'for a; do out=$$a; done' 'cp "$$METEO_TEST_ANSWER" "$$out" && echo "ok 1"' > $@
	chmod +x $@

meteo_alerts_test: meteo_alerts_test.c build/.copied $(SRC)/meteo.c $(SRC)/meteo.h $(SRC)/cJSON.c $(SRC)/qth/qth.c \
                   $(wildcard stub_include/*.h stub_include/*/*.h)
	$(CC) $(NFLAGS) -o $@ meteo_alerts_test.c $(SRC)/meteo.c $(SRC)/cJSON.c $(SRC)/qth/qth.c -lm -Wl,--wrap=time

test: meteo_c_test meteo_alerts_test build/sbin/x6100-meteo
	./meteo_c_test
	rm -rf $(DATA) && mkdir -p $(DATA)
	./meteo_alerts_test
	BOTTLE=$(if $(BOTTLE),$(abspath $(BOTTLE))) python3 -I -B meteo_test.py
	if command -v node > /dev/null; then node meteo_page_test.js; else echo "no node: the page's script not run"; fi

clean:
	rm -rf meteo_c_test meteo_alerts_test build

.PHONY: all test clean
