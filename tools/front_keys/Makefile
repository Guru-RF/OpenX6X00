# Host test of the radio's buttons (src/front_keys.c), issue #233: BAND UP and DOWN step on the press and repeat while
# held, the other keys' long press, and a tap read late by a busy GUI that stays a tap. `make` builds it and runs it.
# src/front_keys.c is compiled from a copy in build/ with stand-ins for the GUI's headers and LVGL's (stub_include/,
# before src/); front_keys.h and gui_events.h are the GUI's own. LVGL's timers and the keypad's reads are run by the
# test on a clock of its own, a FIFO in build/ standing in for the device. See front_keys_test.c for the cases.
TOP    := ../..
SRC    := $(TOP)/src
CFLAGS := -std=gnu11 -Wall -Wextra -Wno-unused-parameter -O1 -g -I stub_include -I $(SRC)

all: test

build/front_keys.c: $(SRC)/front_keys.c
	mkdir -p build && cp $< $@

front_keys_test: front_keys_test.c build/front_keys.c $(SRC)/front_keys.h $(SRC)/gui_events.h \
                 $(wildcard stub_include/*.h stub_include/*/*.h)
	$(CC) $(CFLAGS) -o $@ front_keys_test.c build/front_keys.c ../speech/silent.c

test: front_keys_test
	./front_keys_test

clean:
	rm -rf front_keys_test build

.PHONY: all test clean
