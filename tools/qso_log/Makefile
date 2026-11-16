# Host test of the radio's QSO list (qso_log.db) and the web configurator's Logs page, issue #284: a QSO deleted or
# changed on the page goes from the list FT8, FT4 and JS8 mark the worked calls by, and from the apps' own ADIF files,
# and the apps see it at once. `make` runs both:
# - qso_log_test: src/qso_log.c from a copy in build/ with stand-ins for the GUI's headers (stub_include/, before
#   src/), util_canonize_callsign cut out of src/util.cpp, on a qso_log.db of its own in build/data, with another
#   writer on it as the page is. See qso_log_test.c.
# - logs_follow_test.py: the page's deletes and edits through bottle's WSGI app, on a qso_log.db made with the
#   GUI's own schema (read from src/qso_log.c), ft_log.adi and js8call_log.adi in a new temporary directory.
#   BOTTLE: a folder with bottle.py, by default the radio's own from a buildroot tree built next to this repository
#   (buildroot/local-build.sh), as tools/adif takes it.
TOP    := ../..
SRC    := $(TOP)/src
DATA   := $(CURDIR)/build/data
BOTTLE ?= $(patsubst %/,%,$(dir $(firstword $(wildcard \
              $(TOP)/../AetherX6200Buildroot/build/build/python-bottle-*/bottle.py \
              $(TOP)/../AetherX6100Buildroot/build/build/python-bottle-*/bottle.py))))
CFLAGS := -std=gnu11 -Wall -Wextra -Wno-sign-compare -O1 -g -pthread -I stub_include -I $(SRC) -DBOARD_DATA='"$(DATA)"'

all: test

build/qso_log.c: $(SRC)/qso_log.c
	mkdir -p build && cp $< $@

build/canon.c: $(SRC)/util.cpp
	mkdir -p build
	printf '%s\n' '#include <stdbool.h>' '#include <stdlib.h>' '#include <string.h>' > $@
	awk '/^char \* *util_canonize_callsign\(/,/^}/' $< >> $@
	grep -q 'util_canonize_callsign' $@ || { rm -f $@; echo "util_canonize_callsign not found in $<"; exit 1; }

qso_log_test: qso_log_test.c build/qso_log.c build/canon.c $(SRC)/qso_log.h $(wildcard stub_include/*.h stub_include/*/*.h)
	$(CC) $(CFLAGS) -o $@ qso_log_test.c build/qso_log.c build/canon.c -lsqlite3

test: qso_log_test
	rm -rf $(DATA) && mkdir -p $(DATA)
	./qso_log_test
	BOTTLE=$(if $(BOTTLE),$(abspath $(BOTTLE))) python3 -I -B logs_follow_test.py

clean:
	rm -rf qso_log_test build

.PHONY: all test clean
