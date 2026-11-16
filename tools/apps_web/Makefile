# Host test of the apps' pages of the web configurator (the Apps menu: WeFax, from OlivLoze's fork): what
# webserver apps.py gives them, checked against the GUI's own sources, and through bottle's WSGI app their routes.
# See apps_web_test.py for the cases. `make` runs it; its files go in build/.
# BOTTLE: a folder with bottle.py, by default the radio's own (0.12.23) from a buildroot tree built next to this
# repository, as in tools/ghostnet; without one the WSGI cases are skipped.
TOP    := ../..
BOTTLE ?= $(patsubst %/,%,$(dir $(firstword $(wildcard \
              $(TOP)/../AetherX6200Buildroot/build/build/python-bottle-*/bottle.py \
              $(TOP)/../AetherX6100Buildroot/build/build/python-bottle-*/bottle.py))))

all: test

test:
	rm -rf build && mkdir -p build
	BOTTLE=$(if $(BOTTLE),$(abspath $(BOTTLE))) python3 -I -B apps_web_test.py

clean:
	rm -rf build

.PHONY: all test clean
