# Host test of CapsLock as the keyboard's hyper key (src/kbd_hyper.c, #175): `make` builds it and runs it. See
# kbd_hyper_test.c for the cases.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC)

all: test

kbd_hyper_test: kbd_hyper_test.c $(SRC)/kbd_hyper.c $(SRC)/kbd_hyper.h
	$(CC) $(CFLAGS) -o $@ kbd_hyper_test.c $(SRC)/kbd_hyper.c

test: kbd_hyper_test
	./kbd_hyper_test

clean:
	rm -f kbd_hyper_test

.PHONY: all test clean
