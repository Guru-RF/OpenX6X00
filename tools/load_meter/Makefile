# Host test of the Load meter's sums (src/load_stat.c, #233): /proc/stat's processor lines, /proc/meminfo, a thread's
# stat line, the loads between two reads, the colours, the 10 s peaks, the bars' pixels, and a read of this PC's own
# /proc. `make` builds it and runs it. See load_meter_test.c for the cases.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC)

all: test

load_meter_test: load_meter_test.c $(SRC)/load_stat.c $(SRC)/load_stat.h
	$(CC) $(CFLAGS) -o $@ load_meter_test.c $(SRC)/load_stat.c

test: load_meter_test
	./load_meter_test

clean:
	rm -f load_meter_test

.PHONY: all test clean
