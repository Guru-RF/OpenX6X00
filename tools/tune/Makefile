# Host test of TUNE for an external tuner (src/tune_tx.c, #85 #84): `make` builds and runs it. Its rules (where the
# radio may tune, where the tone goes, what the SWR protection does meanwhile), and tunes sent through the real
# tx_player.c and tx_level.c to a stand-in base that tune_test.c keeps as radio.c sets the real one: where the tone
# lands, the power, the SWR protection, how each tune ends, and everything put back after.
# The sources are copied to build/ first: a quoted #include looks beside the including file before the -I paths,
# and the shims in stub_include must win over src/ (as in tools/tci). The clock is the test's own
# (--wrap=clock_gettime), moved on by the audio played: a 30 s tune takes no 30 s.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -pthread -I stub_include
COPIED := tune_tx.c tune_tx.h tx_player.c tx_player.h tx_level.c tx_level.h swr_guard.c swr_guard.h audio.h radio_types.h
SHIMS  := $(wildcard stub_include/*.h stub_include/*/*.h)

all: test

build/.copied: $(addprefix $(SRC)/,$(COPIED))
	mkdir -p build && cp $^ build/ && touch $@

tune_test: tune_test.c build/.copied $(SHIMS)
	$(CC) $(CFLAGS) -I build -o $@ tune_test.c build/tune_tx.c build/tx_player.c build/tx_level.c build/swr_guard.c \
		-lm -Wl,--wrap=clock_gettime

test: tune_test
	./tune_test

clean:
	rm -rf tune_test build

.PHONY: all test clean
