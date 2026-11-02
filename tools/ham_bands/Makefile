# Host test of src/cfg/ham_bands.c, the amateur bands the radio transmits on by default (#121): `make` builds it and
# runs it. See ham_bands_test.c for the frequencies.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC)

all: test

ham_bands_test: ham_bands_test.c $(SRC)/cfg/ham_bands.c $(SRC)/cfg/ham_bands.h
	$(CC) $(CFLAGS) -o $@ ham_bands_test.c $(SRC)/cfg/ham_bands.c

test: ham_bands_test
	./ham_bands_test

clean:
	rm -f ham_bands_test

.PHONY: all test clean
