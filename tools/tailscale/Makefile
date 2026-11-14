# Host test of the download of Tailscale (rootfs/usr/sbin/x6100-tailscale, download()): `make` runs its own code
# against a web server on 127.0.0.1 serving a small tarball made like Tailscale's, which cuts the link where a case
# says: a try that broke off goes on where it stopped (#262). See download_test.py for the cases.
RUN = mkdir -p build/tmp && TMPDIR=$(CURDIR)/build/tmp python3 -I -B download_test.py

all: test

test:
	$(RUN)

clean:
	rm -rf build

.PHONY: all test clean
