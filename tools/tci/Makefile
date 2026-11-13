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
REVOKED ?= $(CONF).revoked
# REVOKED: while this file exists, the stub's paired browser is revoked (Tailscale Funnel, #58)
# ASK: the stub's "screen", where the radio's question before a client on the network transmits shows while it is
# open; ANSWER: the operator's answer to it, written by the client test. ALLOWED: TCI_ALLOWED, the clients allowed always
ASK     ?= $(CONF).ask
ANSWER  ?= $(CONF).answer
ALLOWED ?= $(CONF).allowed
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -pthread -I stub_include -DTCI_CONF='"$(CONF)"' -DTCI_ALLOWED='"$(ALLOWED)"'
COPIED := tci_server.c tci_server.h tci_audio.c tci_audio.h tci_ws.c tci_ws.h board.h radio_types.h control_owner.c control_owner.h \
          remote_sessions.h tci_ask.h
ENV    := TCI_TEST_PORT=$(PORT) TCI_TEST_PLAYED=$(PLAYED) TCI_TEST_REVOKED=$(REVOKED) TCI_TEST_ASK=$(ASK) \
          TCI_TEST_ANSWER=$(ANSWER) TCI_TEST_ALLOWED=$(ALLOWED)

all: test

build/.copied: $(addprefix $(SRC)/,$(COPIED))
	mkdir -p build && cp $^ build/ && touch $@

tci_audio_test: tci_audio_test.c build/.copied
	$(CC) $(CFLAGS) -O2 -I build -o $@ tci_audio_test.c build/tci_audio.c -lm

audio_test: tci_audio_test
	./tci_audio_test

# Who owns a transmission (audit C01): the server included whole, its socket thread's steps and
# the LVGL thread's work run in the order each case sets (stub_defer), socket pairs for clients
tci_own_test: tci_own_test.c stubs.c build/.copied
	$(CC) $(CFLAGS) -I build -DTCI_STUBS_NO_MAIN -o $@ tci_own_test.c stubs.c build/tci_audio.c build/tci_ws.c build/control_owner.c -lm

own_test: tci_own_test
	./tci_own_test; r=$$?; rm -f $(ALLOWED) $(ALLOWED).tmp; exit $$r

tci_host: stubs.c build/.copied
	$(CC) $(CFLAGS) -I build -o $@ stubs.c build/tci_server.c build/tci_audio.c build/tci_ws.c build/control_owner.c -lm

syntax: build/.copied
	$(CC) $(CFLAGS) -I build -fsyntax-only build/tci_server.c build/tci_audio.c build/tci_ws.c build/control_owner.c

test: audio_test own_test tci_host
	rm -f $(REVOKED) $(ASK) $(ANSWER); $(ENV) ./tci_host & echo $$! > .pid; sleep 0.5; $(ENV) python3 -I tci_client_test.py; r=$$?; kill `cat .pid`; rm -f .pid $(REVOKED) $(ASK) $(ANSWER) $(ALLOWED); exit $$r

clean:
	rm -rf tci_host tci_audio_test tci_own_test .pid build played.raw

.PHONY: all syntax audio_test own_test test clean
