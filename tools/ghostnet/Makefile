# Host test of GhostNet in the JS8 app: src/ghostnet.c (groups, @GSTFLASH, the schedule and the reminder's maths, the
# settings file, the translation helper's answers) against the schedule the image ships, and the translation helper
# rootfs/usr/sbin/x6100-translate with a stand-in for MyMemory (no network). `make` builds them and runs them.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC) -DBOARD_DATA='"$(CURDIR)/data"' \
          -DBOARD_SHARE='"$(CURDIR)/../../rootfs/usr/share/x6100"'

all: test

ghostnet_test: ghostnet_test.c $(SRC)/ghostnet.c $(SRC)/ghostnet.h $(SRC)/ghostnet_iso3.inc
	$(CC) $(CFLAGS) -o $@ ghostnet_test.c $(SRC)/ghostnet.c

test: ghostnet_test
	rm -rf data && mkdir -p data
	./ghostnet_test
	python3 -I translate_test.py

clean:
	rm -rf ghostnet_test data

.PHONY: all test clean
