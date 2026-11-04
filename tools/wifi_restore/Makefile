# Host test of the Backup page's WiFi restore (webserver apps.py): `make` runs it, a backup uploaded and its WiFi
# part restored through bottle's WSGI app, with the web server's own code, against a NetworkManager of this PC.
# It runs in namespaces of its own (unshare -rmn): no network but loopback, NetworkManager's folders on tmpfs and its
# D-Bus a bus of its own, so nothing of this PC's network is touched. The web server's commands find only the
# programs every radio image in TARGETS has, as on the radio: dbus-send, no nmcli. See wifi_restore_test.py.
# Needs unshare (util-linux), NetworkManager, nmcli (the test's own look at NetworkManager) and dbus-daemon.
# BOTTLE: a folder with bottle.py, as in tools/channels. TARGETS: the buildroot target folders of the radio images,
# by default those built next to this repository. KEEP=1 keeps NetworkManager's log (the folder is printed).
# `make v038` runs the same test on v0.3.8's web server, which has to fail it: it called nmcli, which the radios do
# not have, and a restored WiFi network was known only at the next start.
TOP       := ../..
BOTTLE    ?= $(patsubst %/,%,$(dir $(firstword $(wildcard \
                 $(TOP)/../AetherX6200Buildroot/build/build/python-bottle-*/bottle.py \
                 $(TOP)/../AetherX6100Buildroot/build/build/python-bottle-*/bottle.py))))
TARGETS   ?= $(wildcard $(TOP)/../AetherX6200Buildroot/build/target $(TOP)/../AetherX6100Buildroot/build/target)
WEBSERVER ?=
RUN        = mkdir -p build/tmp && TMPDIR=$(CURDIR)/build/tmp BOTTLE=$(if $(BOTTLE),$(abspath $(BOTTLE))) \
             TARGETS="$(abspath $(TARGETS))" WEBSERVER=$(WEBSERVER) unshare -rmn python3 -I -B wifi_restore_test.py

all: test

test:
	$(RUN)

v038: WEBSERVER = $(CURDIR)/build/v038/webserver/src
v038:
	rm -rf build/v038 && mkdir -p build/v038
	git -C $(TOP) archive v0.3.8 webserver/src | tar -x -C build/v038
	if $(RUN) > build/v038.log 2>&1; then echo "v0.3.8 passes the test: it should fail it"; exit 1; fi
	grep -E "^(FAIL|ERROR):|^Ran |^FAILED" build/v038.log
	@echo "v0.3.8 fails the test, as it should"

clean:
	rm -rf build

.PHONY: all test v038 clean
