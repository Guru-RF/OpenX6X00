# Host test of GhostNet in the JS8 app: src/ghostnet.c (groups, @GSTFLASH, the schedule and the reminder's maths, the
# settings file, the translation helper's answers, the messages' log) against the schedule the image ships; the
# translation helper rootfs/usr/sbin/x6100-translate with a stand-in for MyMemory (no network); the web page GhostNet
# (webserver ghostnet_web.py: the radio's time zone, the schedule, the settings, the messages, and through bottle's
# WSGI app its routes). `make` builds them and runs them. BOTTLE: a folder with bottle.py, by default the radio's own
# (0.12.23) from a buildroot tree built next to this repository; without one the WSGI cases are skipped.
TOP    := ../..
SRC    := $(TOP)/src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC) -DBOARD_DATA='"$(CURDIR)/data"' \
          -DBOARD_SHARE='"$(CURDIR)/../../rootfs/usr/share/x6100"'
BOTTLE ?= $(patsubst %/,%,$(dir $(firstword $(wildcard \
              $(TOP)/../AetherX6200Buildroot/build/build/python-bottle-*/bottle.py \
              $(TOP)/../AetherX6100Buildroot/build/build/python-bottle-*/bottle.py))))

all: test

ghostnet_test: ghostnet_test.c $(SRC)/ghostnet.c $(SRC)/ghostnet.h $(SRC)/ghostnet_iso3.inc
	$(CC) $(CFLAGS) -o $@ ghostnet_test.c $(SRC)/ghostnet.c

test: ghostnet_test
	rm -rf data && mkdir -p data
	./ghostnet_test
	python3 -I translate_test.py
	BOTTLE=$(if $(BOTTLE),$(abspath $(BOTTLE))) python3 -I -B ghostnet_web_test.py

clean:
	rm -rf ghostnet_test data

.PHONY: all test clean
