# Host test of the DX cluster's bands, band filter and alerts (src/cluster/spots.c and client.c, issue #103): `make`
# builds it and runs it. CONF is the cluster.conf it writes and reads (/mnt/cluster.conf on the radio), removed after.
SRC    := ../../src
CONF   ?= $(CURDIR)/cluster-test.conf
# client.c cuts lines of the file to its fields on purpose (format-truncation)
CFLAGS := -std=gnu11 -Wall -Wextra -Wno-format-truncation -O1 -g -pthread -I $(SRC) -DCLUSTER_CONF='"$(CONF)"'

all: test

cluster_bands_test: cluster_bands_test.c $(SRC)/cluster/spots.c $(SRC)/cluster/spots.h $(SRC)/cluster/client.c \
                    $(SRC)/cluster/client.h
	$(CC) $(CFLAGS) -o $@ cluster_bands_test.c $(SRC)/cluster/spots.c $(SRC)/cluster/client.c

test: cluster_bands_test
	./cluster_bands_test

clean:
	rm -f cluster_bands_test cluster-test.conf cluster-test.conf.tmp

.PHONY: all test clean
