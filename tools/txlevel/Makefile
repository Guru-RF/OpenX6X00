# Host test of the digital apps' TX level on the X6200 (src/tx_player.c, src/tx_level.c; #126 #123 #16): `make` builds
# and runs it, `make verbose` prints each scenario's transmissions too. The real tx_player.c and tx_level.c send FT8
# transmissions in a row to a model X6200 base (txlevel_test.c): the level learned by one is where the next starts, as
# on the radio. The sources are copied to build/ first: a quoted #include looks beside the including file before the
# -I paths, and the shims in stub_include must win over src/ (as in tools/tune). The clock is the test's own
# (--wrap=clock_gettime), moved on by the audio played: forty transmissions take no ten minutes.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -pthread -I stub_include
COPIED := tx_player.c tx_player.h tx_level.c tx_level.h audio.h radio_types.h
SHIMS  := $(wildcard stub_include/*.h stub_include/*/*.h)
SRCS   := txlevel_test.c build/tx_player.c build/tx_level.c

all: test

build/.copied: $(addprefix $(SRC)/,$(COPIED))
	mkdir -p build && cp $^ build/ && touch $@

txlevel_test: txlevel_test.c build/.copied $(SHIMS)
	$(CC) $(CFLAGS) -I build -o $@ $(SRCS) -lm -Wl,--wrap=clock_gettime

test: txlevel_test
	./txlevel_test

verbose: txlevel_test
	./txlevel_test -v

clean:
	rm -rf txlevel_test build

.PHONY: all test verbose clean
