# Host test of src/base_guard.c, the base's no-data rule (#152): `make` builds it and runs it. See base_guard_test.c
# for the cases.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC)

all: test

base_guard_test: base_guard_test.c $(SRC)/base_guard.c $(SRC)/base_guard.h
	$(CC) $(CFLAGS) -o $@ base_guard_test.c $(SRC)/base_guard.c

test: base_guard_test
	./base_guard_test

clean:
	rm -f base_guard_test

.PHONY: all test clean
