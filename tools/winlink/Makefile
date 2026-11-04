# Host test of the Winlink modem bridge (src/winlink.c): `make` builds it against the stubs and runs the
# protocol test; `make modems` runs the real modems against it. See README.md.
# The sources are copied to build/ first: a quoted #include looks beside the including file before the -I
# paths, and the shims in stub_include must win over src/. The band table (cluster/spots.c, the SWR hold's bands)
# is the real one, in build/cluster/.
# DEPS: liquid-dsp (host/build.sh puts it in build-host/deps). BUILD: the GUI's PC build, where
# `cmake --build $(BUILD) --target ardopcf x6x00-mercury` made the two modems. PAT: a Pat for this PC. XARA: XARA
# for this PC, by default what tools/xara/build.sh --host builds (its pinned Rust, kept in $HOME/.cache/openx6x00;
# the first time it downloads about 150 MB, then it only prints the path); XARA= leaves XARA out
SRC    := ../../src
DEPS   ?= ../../build-host/deps
BUILD  ?= ../../build-host
PAT    ?= pat
XARA   ?= $(shell bash ../xara/build.sh --host)
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -pthread -I stub_include -I $(DEPS)/include
COPIED := winlink.c winlink.h
CLUSTER := cluster/spots.c cluster/spots.h

all: test

build/.copied: $(addprefix $(SRC)/,$(COPIED) $(CLUSTER))
	mkdir -p build/cluster && cp $(addprefix $(SRC)/,$(COPIED)) build/ && cp $(addprefix $(SRC)/,$(CLUSTER)) build/cluster/ && touch $@

winlink_host: stubs.c build/.copied $(wildcard stub_include/*.h stub_include/*/*.h)
	$(CC) $(CFLAGS) -I build -include stub_include/stub_env.h -o $@ stubs.c build/winlink.c build/cluster/spots.c -L $(DEPS)/lib -Wl,-rpath,$(abspath $(DEPS))/lib -lliquid -lm

# ardopcf's clock 10 minutes on at each SIGUSR1 (LD_PRELOAD), for its 10 minute ID in winlink_modems_test.py
clockskip.so: clockskip.c
	$(CC) -std=gnu11 -Wall -Wextra -O1 -pthread -shared -fPIC -o $@ $< -ldl

test: winlink_host
	python3 -I winlink_proto_test.py

# In a network namespace of its own with loopback only (unshare -rn): Pat reaches nothing outside it, no
# Winlink web service, no CMS
modems: winlink_host clockskip.so
	unshare -rn sh -c 'ip link set lo up && python3 -I winlink_modems_test.py --ardopcf $(BUILD)/third-party/ardopcf/ardopcf --mercury $(BUILD)/third-party/mercury/x6x00-mercury --xara "$(XARA)" --pat $(PAT)'

clean:
	rm -rf winlink_host clockskip.so build

.PHONY: all test modems clean
