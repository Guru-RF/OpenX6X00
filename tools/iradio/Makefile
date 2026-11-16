# Host test of the Internet radio (APP > Internet radio): `make` builds and runs it, without the network.
# iradio_list_test.c: the station lines and the favourites file as the window reads them (src/iradio_list.c).
# iradio_test.py: rootfs/usr/sbin/x6100-iradio's lists from saved answers of radio-browser.info (fixtures/), its
# playlists, its decoder choice with ffmpeg and without, its ICY titles and its cache.
# web_test.py: the web configurator's Internet radio page (its favourites API) through bottle's WSGI app, when BOTTLE
# is a folder with bottle.py (the radio's own from a buildroot tree built next to this repository, by default).
SRC     := ../../src
TOP     := ../..
CFLAGS  := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC)
BOTTLE  ?= $(patsubst %/,%,$(dir $(firstword $(wildcard \
               $(TOP)/../AetherX6200Buildroot/build/build/python-bottle-*/bottle.py \
               $(TOP)/../AetherX6100Buildroot/build/build/python-bottle-*/bottle.py))))

all: test

iradio_list_test: iradio_list_test.c $(SRC)/iradio_list.c $(SRC)/iradio_list.h
	$(CC) $(CFLAGS) -o $@ iradio_list_test.c $(SRC)/iradio_list.c

test: iradio_list_test
	./iradio_list_test
	python3 -I -B iradio_test.py
	mkdir -p build/tmp && TMPDIR=$(CURDIR)/build/tmp BOTTLE=$(if $(BOTTLE),$(abspath $(BOTTLE))) \
		python3 -I -B web_test.py

clean:
	rm -rf iradio_list_test build

.PHONY: all test clean
