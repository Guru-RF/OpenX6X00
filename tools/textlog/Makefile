# Host test of the keyboard modes' text log (src/digi_textlog.c, #55): `make` builds and runs it. How the lines are
# put together (the decoder's line break, a pause, a move of the frequency, another mode, a long line, a message
# that grows, what is sent), how they look, the day each goes to, the size limit (the oldest day goes, the newest
# never, nothing past it), and Off. The real module on its own writer thread, into build/data/digi_text.
# The sources are copied to build/ first: a quoted #include looks beside the including file before the -I paths,
# and the shim in stub_include must win over src/ (as in tools/tune). The clock is the test's own (--wrap=time),
# and the card's fsync calls are counted (--wrap=fsync).
# textlog_web_test.py tests the web configurator's side (the Logs page's list, read, download, delete; needs bottle).
SRC    := ../../src
DATA   := $(CURDIR)/build/data
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -pthread -I stub_include -DHOST_BUILD=1 -DBOARD_DATA='"$(DATA)"'
COPIED := digi_textlog.c digi_textlog.h board.h
SHIMS  := $(wildcard stub_include/*/*.h)

all: test

build/.copied: $(addprefix $(SRC)/,$(COPIED))
	mkdir -p build && cp $^ build/ && touch $@

textlog_test: textlog_test.c build/.copied $(SHIMS)
	$(CC) $(CFLAGS) -I build -o $@ textlog_test.c build/digi_textlog.c -lm -Wl,--wrap=time -Wl,--wrap=fsync

test: textlog_test
	rm -rf $(DATA) && mkdir -p $(DATA)
	./textlog_test

clean:
	rm -rf textlog_test build

.PHONY: all test clean
