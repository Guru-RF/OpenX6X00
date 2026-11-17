# Host tests of the Panel stripes fix (#239): `make` builds them and runs them.
# tcon_gamma_test: src/tcon_gamma.h alone, table A and the register steps on a fake TCON page that acts as the
# processor's did on the radio it was tried on.
# panel_stripes_test: src/panel_stripes.c as the GUI runs it, with a device tree and a /dev/mem of the test's own
# (files in test.d) and what it calls of the rest of the GUI stood in for (stub_include). Its sources are copied to
# build/ first: a quoted #include looks beside the including file before the -I paths, and the shims must win over
# src/ (as in tools/gps). See the two .c files.
TOP      := ../..
SRC      := $(TOP)/src
CFLAGS   := -std=gnu11 -O1 -g -Wall -Wextra
COPIED   := panel_stripes.c panel_stripes.h tcon_gamma.h

all: test

tcon_gamma_test: tcon_gamma_test.c $(SRC)/tcon_gamma.h
	$(CC) $(CFLAGS) -I $(SRC) -o $@ tcon_gamma_test.c

build/src/.copied: $(addprefix $(SRC)/,$(COPIED))
	mkdir -p build/src && cp $^ build/src/ && touch $@

panel_stripes_test: panel_stripes_test.c build/src/.copied $(wildcard stub_include/*/*.h stub_include/*/*/*.h)
	$(CC) $(CFLAGS) -Wno-unused-parameter -I stub_include -DUSE_DRM=1 -DDT_TCON='"test.d/compatible"' \
	      -DDEV_MEM='"test.d/mem"' -o $@ panel_stripes_test.c build/src/panel_stripes.c

test: tcon_gamma_test panel_stripes_test
	./tcon_gamma_test
	./panel_stripes_test

clean:
	rm -rf tcon_gamma_test panel_stripes_test build test.d

.PHONY: all test clean
