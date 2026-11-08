# Host test of the broadcast stations' lookup (src/broadcast_db.c): the lookup by the frequency index against the scan
# of the whole list, on the EiBi list the image ships. `make` builds it and runs it. See broadcast_db_test.c.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC) -DBOARD_DATA='"."' -DBOARD_SHARE='"../../rootfs/usr/share/x6100"'

all: test

broadcast_db_test: broadcast_db_test.c $(SRC)/broadcast_db.c $(SRC)/broadcast_db.h
	$(CC) $(CFLAGS) -o $@ broadcast_db_test.c -lm

test: broadcast_db_test
	./broadcast_db_test

clean:
	rm -f broadcast_db_test broadcast.csv

.PHONY: all test clean
