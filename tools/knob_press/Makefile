# Host test of a press of VOL or the MFK, short or held (src/knob_press.h, #282): `make` builds it and runs it. The
# header alone, as screen_main.c takes it for Knob press: next control. See knob_press_test.c.
TOP      := ../..
SRC      := $(TOP)/src
CFLAGS   := -std=gnu11 -O1 -g -Wall -Wextra -I $(SRC)

all: test

knob_press_test: knob_press_test.c $(SRC)/knob_press.h
	$(CC) $(CFLAGS) -o $@ knob_press_test.c

test: knob_press_test
	./knob_press_test

clean:
	rm -f knob_press_test

.PHONY: all test clean
