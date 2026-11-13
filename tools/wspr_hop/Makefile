# Host test of src/wspr/wspr_hop.c, WSPR band hopping (#190): the minute of the hour picks the band, Off bands, 6 m,
# RX only, the ATU rule and never in the middle of a cycle. `make` builds it and runs it. See wspr_hop_test.c.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC)

all: test

wspr_hop_test: wspr_hop_test.c $(SRC)/wspr/wspr_hop.c $(SRC)/wspr/wspr_hop.h $(SRC)/cfg/ham_bands.c
	$(CC) $(CFLAGS) -o $@ wspr_hop_test.c $(SRC)/wspr/wspr_hop.c $(SRC)/cfg/ham_bands.c

test: wspr_hop_test
	./wspr_hop_test

clean:
	rm -f wspr_hop_test

.PHONY: all test clean
