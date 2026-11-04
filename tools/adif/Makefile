# Host test of the FT8 app's log, ft_log.adi (src/adif.c), against the day's log the web configurator's Logs page
# lists and exports (src/field/flog.c), issue #174: `make` builds it and runs it, then adif_web_test.py, the Logs
# page's export through bottle's WSGI app. See adif_test.c and adif_web_test.py for the cases.
# BOTTLE: a folder with bottle.py, by default the radio's own (0.12.23) from a buildroot tree built next to this
# repository (buildroot/local-build.sh), as tools/channels takes it.
SRC    := ../../src
TOP    := ../..
BOTTLE ?= $(patsubst %/,%,$(dir $(firstword $(wildcard \
              $(TOP)/../AetherX6200Buildroot/build/build/python-bottle-*/bottle.py \
              $(TOP)/../AetherX6100Buildroot/build/build/python-bottle-*/bottle.py))))
# adif.c cuts a longer call or name to the day's log's fields, as the day's log does, and keeps an old helper
CFLAGS := -std=gnu11 -Wall -Wextra -Wno-format-truncation -Wno-unused-function -O1 -g -I $(SRC)

all: test

adif_test: adif_test.c $(SRC)/adif.c $(SRC)/adif.h $(SRC)/field/flog.c $(SRC)/field/flog.h
	$(CC) $(CFLAGS) -o $@ adif_test.c $(SRC)/adif.c $(SRC)/field/flog.c

test: adif_test
	rm -rf build && mkdir -p build
	./adif_test
	BOTTLE=$(if $(BOTTLE),$(abspath $(BOTTLE))) python3 -I -B adif_web_test.py

clean:
	rm -rf adif_test build

.PHONY: all test clean
