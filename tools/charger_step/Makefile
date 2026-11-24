# Host test of the Charger setting stepped by a key (src/charger_step.h, #276): `make` builds it and runs it. The
# header alone, as screen_main.c takes it. See charger_step_test.c.
TOP      := ../..
SRC      := $(TOP)/src
CFLAGS   := -std=gnu11 -O1 -g -Wall -Wextra -I $(SRC)

all: test

charger_step_test: charger_step_test.c $(SRC)/charger_step.h
	$(CC) $(CFLAGS) -o $@ charger_step_test.c

test: charger_step_test
	./charger_step_test

clean:
	rm -f charger_step_test

.PHONY: all test clean
