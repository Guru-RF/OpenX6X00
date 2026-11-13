# Host test of src/cfg/tx_ranges.c, the TX ranges (#247): South Korea's table and its edges, the own list's lines and
# file, and how both stand with TX outside ham bands and a transverter. `make` builds it and runs it, on files it
# makes in test.d and removes. See tx_ranges_test.c for the cases.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -pthread -I $(SRC)

all: test

tx_ranges_test: tx_ranges_test.c $(SRC)/cfg/tx_ranges.c $(SRC)/cfg/tx_ranges.h $(SRC)/cfg/ham_bands.c \
                $(SRC)/cfg/ham_bands.h
	$(CC) $(CFLAGS) -o $@ tx_ranges_test.c $(SRC)/cfg/tx_ranges.c $(SRC)/cfg/ham_bands.c

test: tx_ranges_test
	./tx_ranges_test

clean:
	rm -rf tx_ranges_test test.d

.PHONY: all test clean
