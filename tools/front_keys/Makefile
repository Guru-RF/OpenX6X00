# Host test of the radio's buttons (src/front_keys.c), issue #233: BAND UP and DOWN step on the press and repeat while
# held, the other keys' long press, and a tap read late by a busy GUI that stays a tap; and the keys read back after
# the kernel dropped events (src/evdev_sync.c, SYN_DROPPED). `make` builds it and runs it.
# src/front_keys.c is compiled from a copy in build/ with stand-ins for the GUI's headers and LVGL's (stub_include/,
# before src/); front_keys.h, evdev_sync.c and gui_events.h are the GUI's own. LVGL's timers and the keypad's reads
# are run by the test on a clock of its own, a FIFO in build/ standing in for the device; the keys the kernel would
# read back (EVIOCGKEY) are the test's (--wrap=ioctl). See front_keys_test.c for the cases.
TOP    := ../..
SRC    := $(TOP)/src
CFLAGS := -std=gnu11 -Wall -Wextra -Wno-unused-parameter -O1 -g -I stub_include -I $(SRC)

all: test

build/front_keys.c: $(SRC)/front_keys.c
	mkdir -p build && cp $< $@

front_keys_test: front_keys_test.c build/front_keys.c $(SRC)/front_keys.h $(SRC)/gui_events.h \
                 $(SRC)/evdev_sync.c $(SRC)/evdev_sync.h $(wildcard stub_include/*.h stub_include/*/*.h)
	$(CC) $(CFLAGS) -o $@ front_keys_test.c build/front_keys.c $(SRC)/evdev_sync.c ../speech/silent.c \
		-Wl,--wrap=ioctl

test: front_keys_test
	./front_keys_test

clean:
	rm -rf front_keys_test build

.PHONY: all test clean
