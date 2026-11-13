# Host test of the web configurator's stored secrets (webserver apps.py, config_pages.py): `make` runs it. The
# Wavelog key, the QRZ, HOTA and WWFF keys, the Winlink passwords (the page's and Pat's own) and the WiFi
# passwords are stored in a new folder, then every page and every GET answer of the web server's own code is fetched
# through bottle's WSGI app, the Files page followed link by link, and the answers that pass on a helper's or a
# server's words are asked for with stand-in helpers that say the secrets: none of it may hold one. See
# secrets_web_test.py for the cases. The paths of the radio are all moved into the test's folder first.
# BOTTLE: a folder with bottle.py, by default the radio's own (0.12.23) from a buildroot tree built next to this
# repository, as in tools/channels. Python 3.13 and later have no cgi module, which bottle 0.12 imports: the test
# puts a stand-in in its place.
# `make v0310` runs the same test on v0.3.10's web server (git archive into build/), which has to fail it, with no
# error (a test that could not run proves nothing): its Files page handed out the key files and backups/, and what a
# helper said went to the page as it was.
TOP       := ../..
BOTTLE    ?= $(patsubst %/,%,$(dir $(firstword $(wildcard \
                 $(TOP)/../AetherX6200Buildroot/build/build/python-bottle-*/bottle.py \
                 $(TOP)/../AetherX6100Buildroot/build/build/python-bottle-*/bottle.py))))
WEBSERVER ?=
RUN        = mkdir -p build/tmp && TMPDIR=$(CURDIR)/build/tmp BOTTLE=$(if $(BOTTLE),$(abspath $(BOTTLE))) \
             WEBSERVER=$(WEBSERVER) python3 -I -B secrets_web_test.py

all: test

test:
	$(RUN)

v0310: WEBSERVER = $(CURDIR)/build/v0310/webserver/src
v0310:
	rm -rf build/v0310 && mkdir -p build/v0310
	git -C $(TOP) archive v0.3.10 webserver/src | tar -x -C build/v0310
	if $(RUN) > build/v0310.log 2>&1; then echo "v0.3.10 passes the test: it should fail it"; exit 1; fi
	grep -E "^(FAIL|ERROR):|^Ran |^FAILED" build/v0310.log
	if grep -q "^ERROR:" build/v0310.log; then echo "v0.3.10 could not run the test: that proves nothing"; exit 1; fi
	@echo "v0.3.10 fails the test, as it should"

clean:
	rm -rf build

.PHONY: all test v0310 clean
