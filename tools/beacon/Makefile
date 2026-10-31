# Host test of the MSG > CW beacon (#71): `make` builds and runs it. The rules alone (src/cw_beacon.c): QRSS3's
# elements of a text, its bands and frequencies. Then the real src/cw_encoder.c on its own thread as on the radio,
# keying a stand-in key and base (beacon_test.c): each element's time, the gaps where the radio receives, the beacon's
# pause, the TX time limit, and every way it stops (a change of frequency or mode, the PTT key, the radio in TX from
# elsewhere, the limit, a stop asked), the key always let go after.
# The sources are copied to build/ first: a quoted #include looks beside the including file before the -I paths, and
# the shims in stub_include must win over src/ (as in tools/tune). The clock is the test's own (--wrap=clock_gettime,
# nanosleep, usleep): the keyer's sleeps move it on, a beacon of an hour takes no hour.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -pthread -I stub_include
COPIED := cw_encoder.c cw_encoder.h cw_beacon.c cw_beacon.h cw_decoder.h
MORSE  := cw_decoder/morse_code.c cw_decoder/morse_code.h
SHIMS  := $(wildcard stub_include/*.h stub_include/*/*.h)

all: test

build/.copied: $(addprefix $(SRC)/,$(COPIED) $(MORSE))
	mkdir -p build/cw_decoder
	cp $(addprefix $(SRC)/,$(COPIED)) build/
	cp $(addprefix $(SRC)/,$(MORSE)) build/cw_decoder/
	touch $@

beacon_test: beacon_test.c build/.copied $(SHIMS)
	$(CC) $(CFLAGS) -I build -o $@ beacon_test.c build/cw_encoder.c build/cw_beacon.c build/cw_decoder/morse_code.c \
		-Wl,--wrap=clock_gettime,--wrap=nanosleep,--wrap=usleep

test: beacon_test
	./beacon_test

clean:
	rm -rf beacon_test build

.PHONY: all test clean
