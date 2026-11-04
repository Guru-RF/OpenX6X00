# Host tests of the keyboard (#175): `make` builds them and runs them. kbd_hyper_test: CapsLock as the hyper key
# (src/kbd_hyper.c); kbd_keymap_test: the keys with no character, by scancode (src/kbd_keymap.c). See the .c files
# for the cases.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC)

all: test

kbd_hyper_test: kbd_hyper_test.c $(SRC)/kbd_hyper.c $(SRC)/kbd_hyper.h
	$(CC) $(CFLAGS) -o $@ kbd_hyper_test.c $(SRC)/kbd_hyper.c

kbd_keymap_test: kbd_keymap_test.c $(SRC)/kbd_keymap.c $(SRC)/kbd_keymap.h
	$(CC) $(CFLAGS) -o $@ kbd_keymap_test.c $(SRC)/kbd_keymap.c

test: kbd_hyper_test kbd_keymap_test
	./kbd_hyper_test
	./kbd_keymap_test

clean:
	rm -f kbd_hyper_test kbd_keymap_test

.PHONY: all test clean
