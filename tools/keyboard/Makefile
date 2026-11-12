# Host tests of the keyboard (#175): `make` builds them and runs them. kbd_hyper_test: CapsLock as the hyper key and
# its chords (src/kbd_hyper.c); kbd_keymap_test: the keys with no character, by scancode (src/kbd_keymap.c);
# kbd_ptt_test: CapsLock+Space, the PTT toggle (src/kbd_ptt.c); kbd_layout_test: the layouts of Settings > Keys >
# KEYBOARD made by xkb, with libxkbcommon (src/kbd_layout.c, #236): XKB_ROOTS="<target>/usr/share/X11/xkb ..." adds
# the radios' own xkb data to the PC's. See the .c files for the cases.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC)

all: test

kbd_hyper_test: kbd_hyper_test.c $(SRC)/kbd_hyper.c $(SRC)/kbd_hyper.h
	$(CC) $(CFLAGS) -o $@ kbd_hyper_test.c $(SRC)/kbd_hyper.c

kbd_keymap_test: kbd_keymap_test.c $(SRC)/kbd_keymap.c $(SRC)/kbd_keymap.h
	$(CC) $(CFLAGS) -o $@ kbd_keymap_test.c $(SRC)/kbd_keymap.c

kbd_ptt_test: kbd_ptt_test.c $(SRC)/kbd_ptt.c $(SRC)/kbd_ptt.h
	$(CC) $(CFLAGS) -o $@ kbd_ptt_test.c $(SRC)/kbd_ptt.c

kbd_layout_test: kbd_layout_test.c $(SRC)/kbd_layout.c $(SRC)/kbd_layout.h
	$(CC) $(CFLAGS) -o $@ kbd_layout_test.c $(SRC)/kbd_layout.c -lxkbcommon

test: kbd_hyper_test kbd_keymap_test kbd_ptt_test kbd_layout_test
	./kbd_hyper_test
	./kbd_keymap_test
	./kbd_ptt_test
	XKB_ROOTS="$(XKB_ROOTS)" ./kbd_layout_test

clean:
	rm -f kbd_hyper_test kbd_keymap_test kbd_ptt_test kbd_layout_test

.PHONY: all test clean
