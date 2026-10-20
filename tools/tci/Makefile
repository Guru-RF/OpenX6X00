# Host test of the TCI server: `make` builds it against the stubs and runs the client test.
# The two sources are copied to build/ first: a quoted #include looks beside the including
# file before the -I paths, and the shims in stub_include must win over src/.
# The conf file is TCI_CONF, /mnt/tci.conf on the radio: here it is /tmp/tci-test.conf.
SRC    := ../../src
CFLAGS := -std=c11 -Wall -Wextra -O1 -g -pthread -I stub_include -DTCI_CONF='"/tmp/tci-test.conf"'

all: test

build/tci_server.c: $(SRC)/tci_server.c $(SRC)/tci_server.h $(SRC)/tci_ws.h
	mkdir -p build && cp $(SRC)/tci_server.c $(SRC)/tci_server.h $(SRC)/tci_ws.h build/

build/tci_ws.c: $(SRC)/tci_ws.c $(SRC)/tci_ws.h
	mkdir -p build && cp $(SRC)/tci_ws.c $(SRC)/tci_ws.h build/

tci_host: stubs.c build/tci_server.c build/tci_ws.c
	$(CC) $(CFLAGS) -I build -o $@ $^ -lm

syntax: build/tci_server.c build/tci_ws.c
	$(CC) $(CFLAGS) -I build -fsyntax-only build/tci_server.c build/tci_ws.c

test: tci_host
	./tci_host & echo $$! > .pid; sleep 0.5; python3 -I tci_client_test.py; r=$$?; kill `cat .pid`; rm -f .pid; exit $$r

clean:
	rm -rf tci_host .pid build

.PHONY: all syntax test clean
