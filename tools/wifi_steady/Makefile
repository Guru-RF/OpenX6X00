# Host test of rootfs/etc/NetworkManager/dispatcher.d/20-wifi-steady, the hook that turns the supplicant's
# background scans off once a WiFi link is up: `make` runs the hook as it is, against this PC's wpa_supplicant on a
# D-Bus of its own, in namespaces of its own (unshare -rmn: no network but loopback and a dummy link, /run and
# /sys/class/net on tmpfs), so nothing of this PC's network is touched. iw and logger are stand-ins that note what
# they were asked. Needs unshare (util-linux), wpa_supplicant with D-Bus (-u), gdbus (GLib), dbus-daemon and ip.
# See wifi_steady_test.sh for the cases.

all: test

test:
	unshare -rmn sh wifi_steady_test.sh ../../rootfs/etc/NetworkManager/dispatcher.d/20-wifi-steady

.PHONY: all test
