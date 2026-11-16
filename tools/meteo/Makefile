# Host test of the Weather app: `make` runs both.
# - meteo_c_test: src/meteo.c (where the weather is asked for, the helper's arguments, the levels, the times, a warning
#   on one line) with station_position cut out of src/station.c into build/, as on the radio, and stand-ins for the
#   GPS, the locator, the country and the language (meteo_c_test.c). meteo.h's headers want LVGL beside src/.
# - meteo_test.py: the helper (rootfs/usr/sbin/x6100-meteo) with the services' answers saved in samples/, its network
#   a stand-in: nothing goes to any service. BOTTLE: a folder with bottle.py, by default the radio's own from a
#   buildroot tree built next to this repository, as in tools/flights; without it the web page's part is skipped.
TOP    := ../..
SRC    := $(TOP)/src
LVGL   ?= $(TOP)/lvgl
BOTTLE ?= $(patsubst %/,%,$(dir $(firstword $(wildcard \
              $(TOP)/../AetherX6200Buildroot/build/build/python-bottle-*/bottle.py \
              $(TOP)/../AetherX6100Buildroot/build/build/python-bottle-*/bottle.py))))
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE \
          -I $(TOP) -I $(SRC) -isystem $(LVGL)

all: test

build/station_position.c: $(SRC)/station.c
	mkdir -p build
	awk '/^#define (POS_MAX_AGE_S|PHONE_MAX_ACC_M)/; /^bool station_position\(/,/^}/' $< > $@
	grep -q '^bool station_position(' $@ || { rm -f $@; echo "station_position not found in $<"; exit 1; }

meteo_c_test: meteo_c_test.c build/station_position.c $(SRC)/meteo.c $(SRC)/meteo.h $(SRC)/cJSON.c $(SRC)/qth/qth.c
	$(CC) $(CFLAGS) -o $@ meteo_c_test.c $(SRC)/meteo.c $(SRC)/cJSON.c $(SRC)/qth/qth.c -lm

test: meteo_c_test
	./meteo_c_test
	BOTTLE=$(if $(BOTTLE),$(abspath $(BOTTLE))) python3 -I -B meteo_test.py

clean:
	rm -rf meteo_c_test build

.PHONY: all test clean
