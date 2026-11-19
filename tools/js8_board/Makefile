# Host test of the JS8 board's red card (src/js8/js8_logic.h, js8_heard_me_fresh): a station that heard us is red for
# 3 minutes after their last answer, then a plain card with "heard you" in grey (F4OLI). `make` builds and runs it.
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
