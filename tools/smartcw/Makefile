# Host test of SmartCW, RF.Guru's keyer, on the radio's USB port: `make` builds and runs the five parts.
# - smartcw_key_test.c: its MIDI on the USB paddle, the logic of src/paddle_keyer.c alone (PADDLE_IAMBIC_ONLY): its
#   bytes, what each note does, its keyed notes as a straight key, SmartCW found by its USB IDs in a sysfs tree it
#   makes under build/.
# - smartcw_thread_test.c: the keyer thread (src/paddle_keyer.c whole, stand-ins for the radio, the settings and
#   LVGL in stub_include) on a pseudo-terminal as SmartCW's MIDI port, found by its USB IDs in a sysfs tree of the
#   test's: the key with SmartCW's notes in each KEY mode, out of CW, during a CW message, with Learn, the paddle notes
#   through the keyer here (Swap), two sources at once, the SWR protection, unplugged, and another MIDI device by the
#   MIDI map as before. Real time: each case waits for the key with a margin.
# - smartcw_proxy_test.py: SmartCW's page on its own port (webserver smartcw.py) against a stand-in SmartCW on
#   127.0.0.1, the interface it is held to given by the test, and once held to lo by the real SO_BINDTODEVICE: the
#   configurator's rules, one request a connection, the live events as they come, SmartCW not plugged in or not
#   answering. No bottle: the proxy reads its requests itself (the radio's names are the test's). With BOTTLE (a
#   folder with bottle.py, by default the radio's own from a buildroot tree built next to this repository, as in
#   tools/secrets) also the configurator's menu link; skipped without it.
# - smartcw_net_test.py: the udev rule and the NetworkManager profile read as udev and NetworkManager read them, the
#   names they agree on with the GUI and the proxy, and the profile installed 0600 by CMakeLists.txt's own rule.
# - smartcw_nm_test.py: the profile with a NetworkManager of this PC, in namespaces of its own as tools/wifi_restore
#   (unshare -rmn; needs NetworkManager, nmcli, dbus-daemon, dbus-send, ip): left out at 0644, read after an update
#   when the web server tells NetworkManager (smartcw.follow_profile), and taken on smartcw0 before the GUI's own
#   Ethernet profile, which takes another adapter. KEEP=1 keeps NetworkManager's log (the folder is printed).
SRC    := ../../src
TOP    := ../..
BOTTLE ?= $(patsubst %/,%,$(dir $(firstword $(wildcard \
              $(TOP)/../AetherX6200Buildroot/build/build/python-bottle-*/bottle.py \
              $(TOP)/../AetherX6100Buildroot/build/build/python-bottle-*/bottle.py))))
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g
SHIMS  := $(wildcard stub_include/*.h stub_include/*/*.h)

all: test

smartcw_key_test: smartcw_key_test.c $(SRC)/paddle_keyer.c $(SRC)/paddle_keyer.h
	$(CC) $(CFLAGS) -I $(SRC) -DPADDLE_IAMBIC_ONLY -o $@ smartcw_key_test.c $(SRC)/paddle_keyer.c

# Copied first: a quoted #include looks beside the including file before the -I paths, and the shims in stub_include
# must win over src/ (as in tools/beacon)
build/src/.copied: $(SRC)/paddle_keyer.c $(SRC)/paddle_keyer.h
	mkdir -p build/src
	cp $^ build/src/
	touch $@

smartcw_thread_test: smartcw_thread_test.c build/src/.copied $(SHIMS)
	$(CC) $(CFLAGS) -pthread -I build/src -I stub_include -o $@ smartcw_thread_test.c build/src/paddle_keyer.c \
		-Wl,--wrap=glob,--wrap=realpath

test: smartcw_key_test smartcw_thread_test
	rm -rf build/sys build/run build/tmp && mkdir -p build/run build/tmp
	./smartcw_key_test build
	./smartcw_thread_test $(CURDIR)/build/run
	TMPDIR=$(CURDIR)/build/tmp BOTTLE=$(if $(BOTTLE),$(abspath $(BOTTLE))) python3 -I -B smartcw_proxy_test.py
	TMPDIR=$(CURDIR)/build/tmp python3 -I -B smartcw_net_test.py
	TMPDIR=$(CURDIR)/build/tmp unshare -rmn python3 -I -B smartcw_nm_test.py

clean:
	rm -rf smartcw_key_test smartcw_thread_test build

.PHONY: all test clean
