# Host test of the TCI server: `make` builds it against the stubs and runs the client test.
# The sources are copied to build/ first: a quoted #include looks beside the including file
# before the -I paths, and the shims in stub_include must win over src/. The headers that
# need no shim (the board, the radio's vocabulary, who controls the radio) and
# control_owner.c come along as they are.
# The conf file is TCI_CONF, /mnt/tci.conf on the radio: here CONF. PORT is the port the
# server listens on and the client connects to (another one for a second test at once).
# PLAYED: where the stub keeps what the last transmission played, for the client test to read.
SRC    := ../../src
PORT   ?= 50001
CONF   ?= /tmp/tci-test.conf
PLAYED ?= $(CURDIR)/played.raw
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -pthread -I stub_include -DTCI_CONF='"$(CONF)"'
COPIED := tci_server.c tci_server.h tci_ws.c tci_ws.h board.h radio_types.h control_owner.c control_owner.h

all: test

build/.copied: $(addprefix $(SRC)/,$(COPIED))
	mkdir -p build && cp $^ build/ && touch $@

tci_host: stubs.c build/.copied
	$(CC) $(CFLAGS) -I build -o $@ stubs.c build/tci_server.c build/tci_ws.c build/control_owner.c -lm

syntax: build/.copied
	$(CC) $(CFLAGS) -I build -fsyntax-only build/tci_server.c build/tci_ws.c build/control_owner.c

test: tci_host
	TCI_TEST_PORT=$(PORT) TCI_TEST_PLAYED=$(PLAYED) ./tci_host & echo $$! > .pid; sleep 0.5; TCI_TEST_PORT=$(PORT) TCI_TEST_PLAYED=$(PLAYED) python3 -I tci_client_test.py; r=$$?; kill `cat .pid`; rm -f .pid; exit $$r

clean:
	rm -rf tci_host .pid build played.raw

.PHONY: all syntax test clean
