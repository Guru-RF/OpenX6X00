# Host test of what the radio says after a version was taken back (rootfs/usr/sbin/x6100-update): `make` runs it, the
# updater's own code on a folder of its own under build/tmp for each case. The start loop's step back, the one of a
# version that could not use the settings file, the one by hand and the one of an install cut short: the reason and
# the sentence in last.json, "notice" giving it once, "status" giving it, and last.json as earlier updaters wrote it.
# See rollback_test.py for the cases. mkbundle.py beside it makes the update bundle; it is not tested here.
RUN = mkdir -p build/tmp && TMPDIR=$(CURDIR)/build/tmp python3 -I -B rollback_test.py

all: test

test:
	$(RUN)

clean:
	rm -rf build

.PHONY: all test clean
