# Host test of the apps' pages of the web configurator (the Apps menu: FT8, JS8, WeFax, from OlivLoze's fork): what
# webserver apps.py gives them, checked against the GUI's own sources, and through bottle's WSGI app their routes.
# See apps_web_test.py for the cases. `make` runs it; its files go in build/, with dxcc_calls: src/dxcc.c's answer
# for calls, which the pages' own lookup is compared with (under node, when there is one).
# BOTTLE: a folder with bottle.py, by default the radio's own (0.12.23) from a buildroot tree built next to this
# repository, as in tools/ghostnet; without one the WSGI cases are skipped.
TOP    := ../..
BOTTLE ?= $(patsubst %/,%,$(dir $(firstword $(wildcard \
              $(TOP)/../AetherX6200Buildroot/build/build/python-bottle-*/bottle.py \
              $(TOP)/../AetherX6100Buildroot/build/build/python-bottle-*/bottle.py))))

all: test

test:
	rm -rf build && mkdir -p build
	$(CC) -std=gnu11 -Wall -Wextra -O1 -I $(TOP)/src -DCTY_SD='"/nonexistent/cty.dat"' \
	      -DCTY_IMAGE='"$(abspath $(TOP))/third-party/cty/cty.dat"' -o build/dxcc_calls dxcc_calls.c $(TOP)/src/dxcc.c $(TOP)/src/cb.c \
	      -lpthread -lm
	BOTTLE=$(if $(BOTTLE),$(abspath $(BOTTLE))) python3 -I -B apps_web_test.py

clean:
	rm -rf build

.PHONY: all test clean
