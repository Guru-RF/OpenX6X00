# Host test of src/base_guard.c, the base's no-data rules (the notice, the GUI's TX refused) and the base log's one
# line a second (#152): `make` builds it and runs it. See base_guard_test.c for the cases. The log goes to
# base_guard_test.log here, removed after; the clock of the log is the test's own (--wrap=time).
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC) -DBASE_GUARD_LOG='"base_guard_test.log"'

all: test

base_guard_test: base_guard_test.c $(SRC)/base_guard.c $(SRC)/base_guard.h
	$(CC) $(CFLAGS) -o $@ base_guard_test.c $(SRC)/base_guard.c -Wl,--wrap=time

test: base_guard_test
	./base_guard_test

clean:
	rm -f base_guard_test base_guard_test.log base_guard_test.log.1

.PHONY: all test clean
