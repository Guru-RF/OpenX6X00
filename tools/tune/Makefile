# Host test of TUNE for an external tuner (src/tune_tx.c, #85 #84): `make` builds and runs it. Its rules (where the
# radio may tune, where the tone goes, what the SWR protection does meanwhile), and tunes sent through the real
# tx_player.c and tx_level.c to a stand-in base that tune_test.c keeps as radio.c sets the real one: where the tone
# lands, the power, the SWR protection, how each tune ends, and everything put back after. Then the GUI's side, the
# real ext_tune.c on its own thread as on the radio: the refusals and their words, the open app's offset, the stops
# (a change of frequency, mode, VFO or split, the key again, the limit, the Panel's), the button's mark.
# The sources are copied to build/ first: a quoted #include looks beside the including file before the -I paths,
# and the shims in stub_include must win over src/ (as in tools/tci). The clock is the test's own
# (--wrap=clock_gettime), moved on by the audio played: a 30 s tune takes no 30 s. It runs for each radio: TUNE's
# power top is the radio's PWR top (board.h), 10 W on the X6100, 8 W on the X6200 (#87).
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -pthread -I stub_include
COPIED := tune_tx.c tune_tx.h tx_player.c tx_player.h tx_level.c tx_level.h swr_guard.c swr_guard.h audio.h radio_types.h \
          ext_tune.c ext_tune.h board.h
SHIMS  := $(wildcard stub_include/*.h stub_include/*/*.h)
SRCS   := tune_test.c build/tune_tx.c build/tx_player.c build/tx_level.c build/swr_guard.c build/ext_tune.c

all: test

build/.copied: $(addprefix $(SRC)/,$(COPIED))
	mkdir -p build && cp $^ build/ && touch $@

tune_test: tune_test.c build/.copied $(SHIMS)
	$(CC) $(CFLAGS) -I build -o $@ $(SRCS) -lm -Wl,--wrap=clock_gettime

tune_test_x6200: tune_test.c build/.copied $(SHIMS)
	$(CC) $(CFLAGS) -DBOARD_X6200 -I build -o $@ $(SRCS) -lm -Wl,--wrap=clock_gettime

test: tune_test tune_test_x6200
	./tune_test
	./tune_test_x6200

clean:
	rm -rf tune_test tune_test_x6200 build

.PHONY: all test clean
