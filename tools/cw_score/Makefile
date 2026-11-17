# Host test of how the CW Trainer scores (src/cw_trainer_score.c; issue #290): `make` builds it and runs it. Copy's
# typed answer (the keys that type a place, the answer never longer than the group, Backspace) and its check against
# the group, place by place, lined up where a character was missed or typed twice; the on-screen keys the MFK moves
# over; each character's last 20 tries and today's totals (Stats), Learn's and Copy's each; the last 10 groups that
# take the level up at 90 %. See cw_score_test.c for the cases.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC)

all: test

cw_score_test: cw_score_test.c $(SRC)/cw_trainer_score.c $(SRC)/cw_trainer_score.h
	$(CC) $(CFLAGS) -o $@ cw_score_test.c $(SRC)/cw_trainer_score.c

test: cw_score_test
	./cw_score_test

clean:
	rm -f cw_score_test

.PHONY: all test clean
