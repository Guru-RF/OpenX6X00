# Host test of src/wr12.c, the X6200's WR12 driver (#134): `make` builds it and runs it. A stand-in WR12 on a pty
# behind a stand-in sysfs (test.d/), gpsd's control socket too; see wr12_test.c for the cases. The driver's own
# log goes to wr12_test.log here, removed after; its times are cut short so that the keepalive and the retries
# come within the test.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -pthread -I $(SRC) -DBOARD_X6200=1 \
          -DWR12_SYS='"test.d/sys"' -DWR12_DEV='"test.d/dev"' -DWR12_GPSD='"test.d/gpsd.sock"' \
          -DWR12_MARK='"test.d/wr12-out"' -DANSWER_MS=150 -DAYT_MS=400 -DRETRY_MS=300

all: test

wr12_test: wr12_test.c $(SRC)/wr12.c $(SRC)/wr12.h
	$(CC) $(CFLAGS) -o $@ wr12_test.c $(SRC)/wr12.c

test: wr12_test
	./wr12_test

clean:
	rm -rf wr12_test wr12_test.log test.d

.PHONY: all test clean
