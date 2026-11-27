# Host tests of the 3D waterfall's pure parts: src/wf3d_shape.c (a row's levels to trace heights: the noise kept
# as a low smooth base, the signals' peaks kept) and src/closeup_est.c (the close-up's off-by estimate).
# `make` builds and runs them; each prints its failures and a count.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC)
TESTS  := wf3d_shape_test closeup_est_test

all: test

wf3d_shape_test: wf3d_shape_test.c $(SRC)/wf3d_shape.c $(SRC)/wf3d_shape.h
	$(CC) $(CFLAGS) -o $@ wf3d_shape_test.c $(SRC)/wf3d_shape.c

closeup_est_test: closeup_est_test.c $(SRC)/closeup_est.c $(SRC)/closeup_est.h
	$(CC) $(CFLAGS) -o $@ closeup_est_test.c $(SRC)/closeup_est.c -lm

test: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

clean:
	rm -f $(TESTS)

.PHONY: all test clean
