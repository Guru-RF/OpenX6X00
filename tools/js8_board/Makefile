# Host test of the JS8 board's red card and "1 new" (src/js8/js8_logic.h, js8_red_*, js8_msg_is_new): red for a minute
# after a message to us or until selected, a heartbeat's answer neither new nor red (F4OLI). `make` builds and runs it.
TOP    := ../..
SRC    := $(TOP)/src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC)

all: test

js8_board_test: js8_board_test.c $(SRC)/js8/js8_logic.h $(SRC)/js8/js8_receive.h
	$(CC) $(CFLAGS) -o $@ js8_board_test.c

test: js8_board_test
	./js8_board_test

clean:
	rm -f js8_board_test

.PHONY: all test clean
