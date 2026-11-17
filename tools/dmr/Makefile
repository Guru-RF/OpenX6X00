# Host test of the DMR app (APP > DMR): `make` builds and runs them; nothing contacts a DMR network.
# - dmr_ambe_test: src/dmr_ambe.c over mbelib (third-party/mbelib), with ASan and UBSan: the AMBE+2 silence frame in
#   a burst's three places with no FEC error, the burst's layout, frames made with valid FEC that come back bit for
#   bit (and with bit errors corrected), random bursts.
# - dmr_proto_test: src/dmr_proto.c, what the app reads of its helper (the records, the state lines, who talks, the
#   last heard), with ASan and UBSan.
# - dmr_test.py: rootfs/usr/sbin/x6100-dmr, the helper, against a fake master on 127.0.0.1 (fake_master.py) that
#   answers as the MMDVM hotspot protocol does: the login, the options, the keepalive, the logout, the voice frames
#   it passes on and the ones it drops, the refusals, the secrets kept out of everything it writes, and that it
#   sends nothing but those packets (no voice, no data). Any other address fails the test.
SRC    := ../../src
MBE    := ../../third-party/mbelib
CFLAGS := -std=gnu11 -Wall -Wextra -Werror -O1 -g -I $(SRC)
SAN    := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
# mbelib without UBSan's bounds check only: its log2Ml[L + 1] read (third-party/mbelib/UPSTREAM.md), ASan still on
SAN_MBE := $(SAN) -fno-sanitize=bounds
MBE_C  := $(MBE)/mbelib.c $(MBE)/ambe3600x2450.c $(MBE)/ecc.c

all: test

dmr_ambe_test: dmr_ambe_test.c $(SRC)/dmr_ambe.c $(SRC)/dmr_ambe.h $(MBE_C)
	$(CC) $(CFLAGS) $(SAN) -I $(MBE) -c $(SRC)/dmr_ambe.c -o dmr_ambe.o
	$(CC) -std=gnu11 -O1 -g -w $(SAN_MBE) -I $(MBE) -c $(MBE)/mbelib.c -o mbe_mbelib.o
	$(CC) -std=gnu11 -O1 -g -w $(SAN_MBE) -I $(MBE) -c $(MBE)/ambe3600x2450.c -o mbe_ambe.o
	$(CC) -std=gnu11 -O1 -g -w $(SAN_MBE) -I $(MBE) -c $(MBE)/ecc.c -o mbe_ecc.o
	$(CC) $(CFLAGS) $(SAN) -I $(MBE) -o $@ dmr_ambe_test.c dmr_ambe.o mbe_mbelib.o mbe_ambe.o mbe_ecc.o -lm

dmr_proto_test: dmr_proto_test.c $(SRC)/dmr_proto.c $(SRC)/dmr_proto.h
	$(CC) $(CFLAGS) $(SAN) -o $@ dmr_proto_test.c $(SRC)/dmr_proto.c

dmr_proto_dump: dmr_proto_dump.c $(SRC)/dmr_proto.c $(SRC)/dmr_proto.h
	$(CC) $(CFLAGS) -o $@ dmr_proto_dump.c $(SRC)/dmr_proto.c

test: dmr_ambe_test dmr_proto_test dmr_proto_dump
	./dmr_ambe_test
	./dmr_proto_test
	python3 -I -B dmr_test.py

clean:
	rm -f dmr_ambe_test dmr_proto_test dmr_proto_dump *.o

.PHONY: all test clean
