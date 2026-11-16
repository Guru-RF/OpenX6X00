# Host test of the Flights app's helper (rootfs/usr/sbin/x6100-flights) and the web Flights page's API: `make` runs
# it against saved answers of the two services (samples/) served on 127.0.0.1; nothing goes to them. See
# flights_test.py for the cases. BOTTLE: a folder with bottle.py, by default the radio's own from a buildroot tree
# built next to this repository, as in tools/secrets; without it the web part is skipped.
TOP    := ../..
BOTTLE ?= $(patsubst %/,%,$(dir $(firstword $(wildcard \
              $(TOP)/../AetherX6200Buildroot/build/build/python-bottle-*/bottle.py \
              $(TOP)/../AetherX6100Buildroot/build/build/python-bottle-*/bottle.py))))

all: test

test:
	BOTTLE=$(if $(BOTTLE),$(abspath $(BOTTLE))) python3 -I -B flights_test.py

clean:
	true

.PHONY: all test clean
