# Host tests of the channel list: `make` runs both.
# channel_list_test (#346): the GUI's own parser and writer (src/channel_list.c, with src/cJSON.c) on a channels.json
# in build/data, and a recall's order (src/channel_recall.h): the "app" key read, written and round-tripped, a file
# without it, the presets' apps, the app before the tune. See channel_list_test.c for the cases. The sources are
# copied to build/c first: a quoted #include looks beside the including file before the -I paths, and the shims in
# stub_include (radio.h, cfg/cfg_api.h) must win over src/. `make old` builds the same file on an older version's
# src/channel_list.c (OLD, v0.3.13 by default; git archive into build/) and shows what that version does with a file
# that has the key: every channel read, the key gone once it saves.
# The web configurator's Memories page (webserver apps.py, #158): the channel list and
# the MEM slots through bottle's WSGI app, with the web server's own code. channels.json, params.db and the device
# tree's model are in a new folder under build/tmp for each case. See channels_web_test.py for the cases.
# BOTTLE: a folder with bottle.py, by default the radio's own (0.12.23) from a buildroot tree built next to this
# repository (buildroot/local-build.sh); bottle 0.13 does too. Python 3.13 and later have no cgi module, which
# bottle 0.12 imports: the test puts a stand-in in its place.
# `make v038` runs the same test on v0.3.8's web server (git archive into build/), which has to fail it: there one
# channel above 56 MHz emptied the list, and an Add then wrote over all the others.
TOP       := ../..
SRC       := $(TOP)/src
BOTTLE    ?= $(patsubst %/,%,$(dir $(firstword $(wildcard \
                 $(TOP)/../AetherX6200Buildroot/build/build/python-bottle-*/bottle.py \
                 $(TOP)/../AetherX6100Buildroot/build/build/python-bottle-*/bottle.py))))
WEBSERVER ?=
RUN        = mkdir -p build/tmp && TMPDIR=$(CURDIR)/build/tmp BOTTLE=$(if $(BOTTLE),$(abspath $(BOTTLE))) \
             WEBSERVER=$(WEBSERVER) python3 -I -B channels_web_test.py
OLD       ?= v0.3.13
CFLAGS    := -std=gnu11 -Wall -O1 -g -DBOARD_DATA='"build/data"' -I stub_include
COPIED    := channel_list.c channel_list.h channel_recall.h cJSON.c cJSON.h board.h radio_types.h

all: test

build/c/.copied: $(addprefix $(SRC)/,$(COPIED))
	mkdir -p build/c build/data && cp $^ build/c/ && touch $@

build/channel_list_test: channel_list_test.c build/c/.copied $(wildcard stub_include/*.h stub_include/*/*.h)
	$(CC) $(CFLAGS) -I build/c -o $@ channel_list_test.c build/c/channel_list.c build/c/cJSON.c -lm

list: build/channel_list_test
	build/channel_list_test

web:
	$(RUN)

test: list web

# The newest board.h and radio_types.h do for the older channel_list.c: it takes the data folder and the modes of them
old:
	rm -rf build/old && mkdir -p build/old build/data
	git -C $(TOP) archive $(OLD) src/channel_list.c src/channel_list.h src/cJSON.c src/cJSON.h | tar -x -C build/old
	cp $(SRC)/board.h $(SRC)/radio_types.h build/old/src/
	$(CC) $(CFLAGS) -DOLD_GUI -I build/old/src -o build/old/channel_list_test channel_list_test.c \
	    build/old/src/channel_list.c build/old/src/cJSON.c -lm
	build/old/channel_list_test

v038: WEBSERVER = $(CURDIR)/build/v038/webserver/src
v038:
	rm -rf build/v038 && mkdir -p build/v038
	git -C $(TOP) archive v0.3.8 webserver/src | tar -x -C build/v038
	if $(RUN) > build/v038.log 2>&1; then echo "v0.3.8 passes the test: it should fail it"; exit 1; fi
	grep -E "^(FAIL|ERROR):|^Ran |^FAILED" build/v038.log
	@echo "v0.3.8 fails the test, as it should"

clean:
	rm -rf build

.PHONY: all test list web old v038 clean
