# Host test of the WSJT-X UDP output for QSOs logged outside the FT8 window (src/ft8_udp.c, ft8_udp_log_record,
# #248): `make` builds and runs it. The real module sends to a socket of the test's own on 127.0.0.1; the test reads
# "QSO Logged" (type 5) and "Logged ADIF" (type 12) back field by field, and checks enabled=0 (nothing), a socket of
# its own closed again with the FT8 window shut, the window's socket kept while it is open, and a record too long for
# a datagram (type 5 alone). The sources are copied to build/ first, as in tools/textlog, and both are made again at
# every run: the data directory is compiled in, and a tree copied elsewhere would read the old one's.
SRC    := ../../src
DATA   := $(CURDIR)/build/data
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -DHOST_BUILD=1 -DBOARD_DATA='"$(DATA)"'
COPIED := ft8_udp.c ft8_udp.h board.h

all: test

build/.copied: $(addprefix $(SRC)/,$(COPIED))
	mkdir -p build && cp $^ build/ && touch $@

ft8_udp_test: ft8_udp_test.c build/.copied
	$(CC) $(CFLAGS) -I build -o $@ ft8_udp_test.c build/ft8_udp.c

test: ft8_udp_test
	rm -rf $(DATA) && mkdir -p $(DATA)
	./ft8_udp_test $(DATA)

clean:
	rm -rf ft8_udp_test build

.PHONY: all test clean build/.copied ft8_udp_test
