# Host test of src/cfg/lo_offset.c, where the waterfall and the spectrum place the base's frames in receive and in TX
# (#191): `make` builds it and runs it. Receive as before on every base, TX on Xiegu's 1.1.9 the receive offset over
# the zoom, checked against the carrier as the reporter's screen showed it. See lo_offset_test.c.
TOP      := ../..
SRC      := $(TOP)/src
CFLAGS   := -std=gnu11 -O1 -g -Wall -Wextra -I $(SRC)

all: test

lo_offset_test: lo_offset_test.c $(SRC)/cfg/lo_offset.c $(SRC)/cfg/lo_offset.h $(SRC)/radio_types.h
	$(CC) $(CFLAGS) -o $@ lo_offset_test.c $(SRC)/cfg/lo_offset.c -lm

test: lo_offset_test
	./lo_offset_test

clean:
	rm -f lo_offset_test

.PHONY: all test clean
