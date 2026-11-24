# Host test of the USB Bluetooth dongle (from OlivLoze's fork, F4OLI): `make` builds and runs it. bt_pick_test.c: the
# adapter the GUI takes (src/bt_pick.h, header only, as bt_dev.c takes it); bt_dongle_test.py: rootfs/usr/sbin/bt-dongle
# against a stand-in sysfs and btaudio-ctl sync against a stand-in pactl, all in build/ (removed after a pass).
TOP      := ../..
SRC      := $(TOP)/src
CFLAGS   := -std=gnu11 -O1 -g -Wall -Wextra -I $(SRC)

all: test

bt_pick_test: bt_pick_test.c $(SRC)/bt_pick.h
	$(CC) $(CFLAGS) -o $@ bt_pick_test.c

test: bt_pick_test
	./bt_pick_test
	python3 -I -B bt_dongle_test.py

clean:
	rm -rf bt_pick_test build

.PHONY: all test clean
