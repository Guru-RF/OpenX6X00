# Host test of Wavelog's live link (#245), both ways: `make` builds the GUI's side and runs it, then the web server's.
# wavelog_cat_test.c: src/wavelog_cat.c (what the radio is on, and a tune from a click in Wavelog) with control_owner.c
# and the quick QSO log's modes (src/field/flog.c) as they are, the rest stood in for by stub_include. The two sources
# are copied to build/ first: a quoted #include looks beside the including file before the -I paths, and the shims
# must win over src/. The shims have no PTT and no modem: a tune that keyed the radio would not link.
# wavelog_live_test.py: the web server's side (webserver wavelog_live.py) through bottle's WSGI app, a stand-in Wavelog
# for api/radio and a stand-in GUI on the configurator socket. See each file for the cases.
# BOTTLE: a folder with bottle.py, by default the radio's own (0.12.23) from a buildroot tree built next to this
# repository, as in tools/secrets; Python 3.13 and later have no cgi module, which bottle 0.12 imports: the test puts
# a stand-in in its place.
TOP    := ../..
SRC    := $(TOP)/src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -pthread -I stub_include -I $(SRC)
COPIED := wavelog_cat.c control_owner.c
BOTTLE ?= $(patsubst %/,%,$(dir $(firstword $(wildcard \
              $(TOP)/../AetherX6200Buildroot/build/build/python-bottle-*/bottle.py \
              $(TOP)/../AetherX6100Buildroot/build/build/python-bottle-*/bottle.py))))

all: test

build/.copied: $(addprefix $(SRC)/,$(COPIED))
	mkdir -p build && cp $(addprefix $(SRC)/,$(COPIED)) build/ && touch $@

wavelog_cat_test: wavelog_cat_test.c build/.copied $(SRC)/wavelog_cat.h $(SRC)/control_owner.h $(SRC)/field/flog.c \
                  $(SRC)/field/flog.h $(wildcard stub_include/*.h stub_include/cfg/*.h)
	$(CC) $(CFLAGS) -o $@ wavelog_cat_test.c build/wavelog_cat.c build/control_owner.c $(SRC)/field/flog.c

test: wavelog_cat_test
	./wavelog_cat_test
	mkdir -p build/tmp && TMPDIR=$(CURDIR)/build/tmp BOTTLE=$(if $(BOTTLE),$(abspath $(BOTTLE))) \
	    python3 -I -B wavelog_live_test.py

clean:
	rm -rf wavelog_cat_test build

.PHONY: all test clean
