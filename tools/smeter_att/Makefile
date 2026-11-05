# Host test of src/cfg/smeter_att.c, the S-meter on a band never set (#125): the X6200's attenuator figure and the cal
# start of both radios. `make` builds it and runs it on the band list of sql/params.sql and the figures in
# docs/MANUAL.md. See smeter_att_test.c for the checks.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC)

all: test

smeter_att_test: smeter_att_test.c $(SRC)/cfg/smeter_att.c $(SRC)/cfg/smeter_att.h
	$(CC) $(CFLAGS) -o $@ smeter_att_test.c $(SRC)/cfg/smeter_att.c

test: smeter_att_test
	./smeter_att_test ../../sql/params.sql ../../docs/MANUAL.md

clean:
	rm -f smeter_att_test

.PHONY: all test clean
