# Host test of the web configurator's Memories page (webserver apps.py, #158): `make` runs it, the channel list and
# the MEM slots through bottle's WSGI app, with the web server's own code. channels.json, params.db and the device
# tree's model are in a new folder under build/tmp for each case. See channels_web_test.py for the cases.
# BOTTLE: a folder with bottle.py, by default the radio's own (0.12.23) from a buildroot tree built next to this
# repository (buildroot/local-build.sh); bottle 0.13 does too. Python 3.13 and later have no cgi module, which
# bottle 0.12 imports: the test puts a stand-in in its place.
# `make v038` runs the same test on v0.3.8's web server (git archive into build/), which has to fail it: there one
# channel above 56 MHz emptied the list, and an Add then wrote over all the others.
TOP       := ../..
BOTTLE    ?= $(patsubst %/,%,$(dir $(firstword $(wildcard \
                 $(TOP)/../AetherX6200Buildroot/build/build/python-bottle-*/bottle.py \
                 $(TOP)/../AetherX6100Buildroot/build/build/python-bottle-*/bottle.py))))
WEBSERVER ?=
RUN        = mkdir -p build/tmp && TMPDIR=$(CURDIR)/build/tmp BOTTLE=$(if $(BOTTLE),$(abspath $(BOTTLE))) \
             WEBSERVER=$(WEBSERVER) python3 -I -B channels_web_test.py

all: test

test:
	$(RUN)

v038: WEBSERVER = $(CURDIR)/build/v038/webserver/src
v038:
	rm -rf build/v038 && mkdir -p build/v038
	git -C $(TOP) archive v0.3.8 webserver/src | tar -x -C build/v038
	if $(RUN) > build/v038.log 2>&1; then echo "v0.3.8 passes the test: it should fail it"; exit 1; fi
	grep -E "^(FAIL|ERROR):|^Ran |^FAILED" build/v038.log
	@echo "v0.3.8 fails the test, as it should"

clean:
	rm -rf build

.PHONY: all test v038 clean
