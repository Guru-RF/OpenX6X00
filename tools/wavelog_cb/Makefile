# Host test of the Wavelog upload with CB QSOs (#286): `make` runs it. rootfs/usr/sbin/x6100-wavelog's collect() on
# logs written in a new folder: a QSO with a CB call on either side (its call, STATION_CALLSIGN or OPERATOR) never goes
# into the queue, an amateur one goes as before. See wavelog_cb_test.py.
all: test

test:
	python3 -I -B wavelog_cb_test.py

clean:

.PHONY: all test clean
