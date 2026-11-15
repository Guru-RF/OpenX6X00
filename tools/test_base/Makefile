# Host test of the X6100 test base "tx1a2-test" (src/hw/x6100/test_base.c): the exact version string it is known by,
# the bits of registers 25, 36 and 38, and the order its settings go to the base in (register 36 without the token,
# 25 and 38 whole, 36 with the token last), held while the radio transmits, stopped at a failed write. `make` builds
# it and runs it. See test_base_test.c for the cases.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -Werror -O1 -g -pthread -I $(SRC)/hw/x6100

all: test

test_base_test: test_base_test.c $(SRC)/hw/x6100/test_base.c $(SRC)/hw/x6100/test_base.h
	$(CC) $(CFLAGS) -o $@ test_base_test.c $(SRC)/hw/x6100/test_base.c -lm

test: test_base_test
	./test_base_test

clean:
	rm -f test_base_test

.PHONY: all test clean
