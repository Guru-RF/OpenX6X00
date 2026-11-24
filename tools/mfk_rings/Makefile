# Host test of the X6200's two MFK rings (src/mfk_rings.h, #354): `make` builds it and runs it. The header alone, as
# mfk_device.c and knob_device.c take it for MFK rings: Stock / Swapped. See mfk_rings_test.c.
TOP      := ../..
SRC      := $(TOP)/src
CFLAGS   := -std=gnu11 -O1 -g -Wall -Wextra -I $(SRC)

all: test

mfk_rings_test: mfk_rings_test.c $(SRC)/mfk_rings.h
	$(CC) $(CFLAGS) -o $@ mfk_rings_test.c

test: mfk_rings_test
	./mfk_rings_test

clean:
	rm -f mfk_rings_test

.PHONY: all test clean
