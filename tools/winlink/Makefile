# Host test of the Winlink modem bridge (src/winlink.c): `make` builds it against the stubs and runs the
# protocol test; `make modems` runs the real modems against it. See README.md.
# The sources are copied to build/ first: a quoted #include looks beside the including file before the -I
# paths, and the shims in stub_include must win over src/.
# DEPS: liquid-dsp (host/build.sh puts it in build-host/deps). BUILD: the GUI's PC build, where
# `cmake --build $(BUILD) --target ardopcf x6x00-mercury` made the two modems. PAT: a Pat for this PC.
SRC    := ../../src
DEPS   ?= ../../build-host/deps
BUILD  ?= ../../build-host
PAT    ?= pat
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -pthread -I stub_include -I $(DEPS)/include
COPIED := winlink.c winlink.h

all: test

build/.copied: $(addprefix $(SRC)/,$(COPIED))
	mkdir -p build && cp $^ build/ && touch $@

winlink_host: stubs.c build/.copied $(wildcard stub_include/*.h stub_include/*/*.h)
	$(CC) $(CFLAGS) -I build -include stub_include/stub_env.h -o $@ stubs.c build/winlink.c -L $(DEPS)/lib -Wl,-rpath,$(abspath $(DEPS))/lib -lliquid -lm

test: winlink_host
	python3 -I winlink_proto_test.py

# In a network namespace of its own with loopback only (unshare -rn): Pat reaches nothing outside it, no
# Winlink web service, no CMS
modems: winlink_host
	unshare -rn sh -c 'ip link set lo up && python3 -I winlink_modems_test.py --ardopcf $(BUILD)/third-party/ardopcf/ardopcf --mercury $(BUILD)/third-party/mercury/x6x00-mercury --pat $(PAT)'

clean:
	rm -rf winlink_host build

.PHONY: all test modems clean
