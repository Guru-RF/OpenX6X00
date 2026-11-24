# Host test of the digit the tuning step underlines in the frequency strip (src/freq_strip.c, freq_strip_step_char())
# in the two forms of Settings > Display > MAIN SCREEN > Frequency, 14.074.000 and 14.074000 (#344): `make` builds it
# and runs it. That function alone, as FREQ_STRIP_STEP_ONLY leaves it. See freq_form_test.c.
TOP      := ../..
SRC      := $(TOP)/src
CFLAGS   := -std=gnu11 -O1 -g -Wall -Wextra -DFREQ_STRIP_STEP_ONLY

all: test

freq_form_test: freq_form_test.c $(SRC)/freq_strip.c
	$(CC) $(CFLAGS) -o $@ freq_form_test.c $(SRC)/freq_strip.c

test: freq_form_test
	./freq_form_test

clean:
	rm -f freq_form_test

.PHONY: all test clean
