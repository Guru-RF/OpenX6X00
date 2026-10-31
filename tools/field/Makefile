# Host test of Field Ops' references (src/field/flog.c, issue #81): `make` builds it and runs it. See
# field_refs_test.c for the cases.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC)

all: test

field_refs_test: field_refs_test.c $(SRC)/field/flog.c $(SRC)/field/flog.h
	$(CC) $(CFLAGS) -o $@ field_refs_test.c $(SRC)/field/flog.c

test: field_refs_test
	./field_refs_test

clean:
	rm -f field_refs_test

.PHONY: all test clean
