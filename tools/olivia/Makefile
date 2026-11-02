# Host test of the Olivia and Contestia modem (src/olivia*.c): `make` builds and runs it. Golden hashes of what Olivia
# sends, then text through the modulator, noise and a tuning error into the receiver. No radio, no LVGL.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O2 -g -I $(SRC) -I .
SRCS   := olivia_test.c $(SRC)/olivia.c $(SRC)/olivia_mod.c $(SRC)/olivia_demod.c $(SRC)/olivia_rx.c
DEPS   := $(SRCS) olivia_golden.inc $(wildcard $(SRC)/olivia*.h)

all: test

olivia_test: $(DEPS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lm

test: olivia_test
	./olivia_test

clean:
	rm -f olivia_test

.PHONY: all test clean
