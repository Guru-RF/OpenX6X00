# Host test of src/bat_watch.c, the battery readings radio.c takes (#160): when the low-battery power-off starts and
# stops, on the X6100 and on the X6200 (BOARD_X6200), each built from the same test. `make` builds both and runs them.
# See bat_watch_test.c for the cases.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC)
DEPS   := bat_watch_test.c $(SRC)/bat_watch.c $(SRC)/bat_watch.h $(SRC)/board.h

all: test

bat_watch_test_x6100: $(DEPS)
	$(CC) $(CFLAGS) -o $@ bat_watch_test.c $(SRC)/bat_watch.c

bat_watch_test_x6200: $(DEPS)
	$(CC) $(CFLAGS) -DBOARD_X6200 -o $@ bat_watch_test.c $(SRC)/bat_watch.c

test: bat_watch_test_x6100 bat_watch_test_x6200
	./bat_watch_test_x6100
	./bat_watch_test_x6200

clean:
	rm -f bat_watch_test_x6100 bat_watch_test_x6200

.PHONY: all test clean
