# Host test of the screensaver's and the screen off's times (src/screen_idle.c): when the screensaver comes and when
# the screen goes dark, from the settings, the use, TX, POWER and a viewer from afar; and of the update that keeps a
# radio's screen off time (src/params/migrations.c, _16), run on a params.db made from sql/params.sql. `make` builds
# it and runs it. See screen_idle_test.c for the cases.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -Wno-format -O1 -g -I $(SRC)

all: test

screen_idle_test: screen_idle_test.c $(SRC)/screen_idle.c $(SRC)/screen_idle.h $(SRC)/params/migrations.c
	$(CC) $(CFLAGS) -o $@ screen_idle_test.c $(SRC)/screen_idle.c $(SRC)/params/migrations.c -lsqlite3

test: screen_idle_test
	./screen_idle_test ../../sql/params.sql

clean:
	rm -f screen_idle_test

.PHONY: all test clean
