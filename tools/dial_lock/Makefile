# Host test of the frequency lock kept across a restart (src/dial_lock.h, #342): `make` builds it and runs it. The
# header alone, as screen_main.c takes it. See dial_lock_test.c.
TOP      := ../..
SRC      := $(TOP)/src
CFLAGS   := -std=gnu11 -O1 -g -Wall -Wextra -I $(SRC)

all: test

dial_lock_test: dial_lock_test.c $(SRC)/dial_lock.h
	$(CC) $(CFLAGS) -o $@ dial_lock_test.c

test: dial_lock_test
	./dial_lock_test

clean:
	rm -f dial_lock_test

.PHONY: all test clean
