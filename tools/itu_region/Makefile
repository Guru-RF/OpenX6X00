# Host test of src/itu_region.c, the ITU region of a position (the band plan's Auto): `make` builds it with
# src/qth/qth.c (the locators) and runs it. See itu_region_test.c for the places.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC)

all: test

itu_region_test: itu_region_test.c $(SRC)/itu_region.c $(SRC)/itu_region.h $(SRC)/qth/qth.c $(SRC)/qth/qth.h
	$(CC) $(CFLAGS) -o $@ itu_region_test.c $(SRC)/itu_region.c $(SRC)/qth/qth.c -lm

test: itu_region_test
	./itu_region_test

clean:
	rm -f itu_region_test

.PHONY: all test clean
