# Host test of the CTCSS tone (src/ctcss.c, #141): `make` builds and runs it.
# - ctcss_test: the 50 tones and the steps through them with Off between the ends, each tone's frequency and level as
#   the samples show them (the two measured levels exactly), no click from one block to the next, the audio under the
#   tone unchanged, no tone when off.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -Werror -O1 -g -I $(SRC)

all: test

ctcss_test: ctcss_test.c $(SRC)/ctcss.c $(SRC)/ctcss.h
	$(CC) $(CFLAGS) -o $@ ctcss_test.c $(SRC)/ctcss.c -lm

test: ctcss_test
	./ctcss_test

clean:
	rm -f ctcss_test

.PHONY: all test clean
