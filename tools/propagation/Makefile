# Host test of the Propagation and Scanner apps' engines, without the GUI: where the sun is (src/sun.c, against
# the 2024 solstice and equinox and a local noon), the NCDXF schedule and the scoring of beacon slots
# (src/beacons.c: one beacon heard among noise, a band open wide, a slot that changed band), and the scanner's
# stops, hang time, lockouts and learning (src/scan.c). `make` builds it and runs it, for each radio.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC)
SRCS   := prop_test.c $(SRC)/sun.c $(SRC)/beacons.c $(SRC)/scan.c
DEPS   := $(SRCS) $(SRC)/sun.h $(SRC)/beacons.h $(SRC)/scan.h $(SRC)/board.h

all: test

prop_test: $(DEPS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lm

prop_test_x6200: $(DEPS)
	$(CC) $(CFLAGS) -DBOARD_X6200 -o $@ $(SRCS) -lm

test: prop_test prop_test_x6200
	./prop_test
	./prop_test_x6200

clean:
	rm -f prop_test prop_test_x6200

.PHONY: all test clean
