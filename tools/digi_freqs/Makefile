# Host test of the frequency lists of PSK, Olivia and JTTY (src/digi_freqs.c, #205): `make` builds it and runs it, then
# the web configurator's side. digi_freqs_test.c: the real digi_freqs.c and amateur band table (src/cfg/ham_bands.c),
# the band plan's region and the user's rows of params.db stood in for; band_plan.h includes "lvgl/lvgl.h", LVGL's
# headers beside src/ (LVGL: their folder). digi_freqs_web_test.py: the Digital modes page's app types against
# src/cfg/digital_modes.h, and through bottle's WSGI app the user's rows of the three apps and the lists from the GUI.
# BOTTLE: a folder with bottle.py, by default the radio's own (0.12.23) from a buildroot tree built next to this
# repository (buildroot/local-build.sh); without one the WSGI cases are skipped (the line says "skipped") and the type
# check still runs.
TOP    := ../..
SRC    := $(TOP)/src
LVGL   ?= $(TOP)/lvgl
DEFS   := -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g $(DEFS) -I $(TOP) -I $(SRC) -isystem $(dir $(abspath $(LVGL)))
BOTTLE ?= $(patsubst %/,%,$(dir $(firstword $(wildcard \
              $(TOP)/../AetherX6200Buildroot/build/build/python-bottle-*/bottle.py \
              $(TOP)/../AetherX6100Buildroot/build/build/python-bottle-*/bottle.py))))

all: test

digi_freqs_test: digi_freqs_test.c $(SRC)/digi_freqs.c $(SRC)/digi_freqs.h $(SRC)/cfg/ham_bands.c \
                 $(SRC)/cfg/ham_bands.h $(SRC)/cfg/digital_modes.h
	$(CC) $(CFLAGS) -o $@ digi_freqs_test.c $(SRC)/digi_freqs.c $(SRC)/cfg/ham_bands.c

test: digi_freqs_test
	./digi_freqs_test
	mkdir -p build/tmp && TMPDIR=$(CURDIR)/build/tmp BOTTLE=$(if $(BOTTLE),$(abspath $(BOTTLE))) \
	    python3 -I -B digi_freqs_web_test.py

clean:
	rm -rf digi_freqs_test build

.PHONY: all test clean
