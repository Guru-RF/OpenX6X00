# Host test of src/pa_bias.c, the X6200's PA bias from its base's calibration block (#173): `make` builds it and runs
# it. See pa_bias_test.c for the cases.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC) -DBOARD_X6200=1

all: test

pa_bias_test: pa_bias_test.c $(SRC)/pa_bias.c $(SRC)/pa_bias.h
	$(CC) $(CFLAGS) -o $@ pa_bias_test.c $(SRC)/pa_bias.c

test: pa_bias_test
	./pa_bias_test

clean:
	rm -f pa_bias_test

.PHONY: all test clean
