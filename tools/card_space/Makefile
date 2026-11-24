# Host test of the room left on the card (src/card_space.c, #341): when a start warns, from the bytes free, the
# partition's size and the Low space warning choice; the megabytes of each choice; the sizes as shown; and the read
# of a filesystem. `make` builds it and runs it. See card_space_test.c for the cases.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC)

all: test

card_space_test: card_space_test.c $(SRC)/card_space.c $(SRC)/card_space.h
	$(CC) $(CFLAGS) -o $@ card_space_test.c $(SRC)/card_space.c

test: card_space_test
	./card_space_test

clean:
	rm -f card_space_test

.PHONY: all test clean
