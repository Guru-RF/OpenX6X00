# Host test of Field Ops' references and log modes (src/field/flog.c, issues #81, #154): `make` builds it and runs
# it. See field_refs_test.c for the cases; x6100_field_test.py checks rootfs/usr/sbin/x6100-field's list readers.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC)

all: test

field_refs_test: field_refs_test.c $(SRC)/field/flog.c $(SRC)/field/flog.h
	$(CC) $(CFLAGS) -o $@ field_refs_test.c $(SRC)/field/flog.c

test: field_refs_test
	./field_refs_test
	python3 x6100_field_test.py

clean:
	rm -f field_refs_test

.PHONY: all test clean
