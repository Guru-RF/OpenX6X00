# Host test of how the CW Tuner's bars and the decoder's speed fit the frequency strip (src/cw_strip_fit.h, #308):
# `make` builds it and runs it. The header alone, as freq_strip.c takes it: the forms in their order for the room the
# strip has, at the sizes of its fonts, and that what is placed stays inside that room. See cw_strip_test.c.
TOP      := ../..
SRC      := $(TOP)/src
CFLAGS   := -std=gnu11 -O1 -g -Wall -Wextra -I $(SRC)

all: test

cw_strip_test: cw_strip_test.c $(SRC)/cw_strip_fit.h
	$(CC) $(CFLAGS) -o $@ cw_strip_test.c

test: cw_strip_test
	./cw_strip_test

clean:
	rm -f cw_strip_test

.PHONY: all test clean
