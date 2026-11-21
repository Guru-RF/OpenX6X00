# Host test of RTTY's transmitter (src/rtty_tx.c, #312) and its macros (src/rtty_macro.c): `make` builds it and runs
# it. ITA2 with its shifts, read back by receivers that unshift on space and that don't; the tones with Reverse; the
# presets (Ham, Weather and the defaults); the waveform's idle, diddles, framing and tail bit by bit (Goertzel), its
# length, the idle's frequency, the phase across the bit edges, the fades, the blocks; the macro file. No radio, no
# LVGL.
SRC    := ../../src
# the macro file in a folder the test makes: rtty_macro.c takes its name from the test (test_path.h); params.c for
# the defaults the Weather preset must be
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC) -DRTTY_MACROS_PATH=rtty_test_macros_path -include test_path.h \
          -DPARAMS_C='"$(SRC)/params/params.c"'
SRCS   := rtty_tx_test.c $(SRC)/rtty_tx.c $(SRC)/rtty_macro.c

all: test

rtty_tx_test: $(SRCS) $(SRC)/rtty_tx.h $(SRC)/rtty_macro.h test_path.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lm

test: rtty_tx_test
	./rtty_tx_test

clean:
	rm -f rtty_tx_test

.PHONY: all test clean
