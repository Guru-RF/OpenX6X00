# Host test of the hidden apps (src/cfg/apps_list.c, #351): the setting's text as a set and back, names it does not
# know, Settings never hidden, an app added later shown, and the APP button pages closing up, with their number of
# pages. `make` builds it and runs it. See apps_hide_test.c for the cases; the APP grid and the bar itself
# (src/apps_menu.c, src/button_bar.cpp) draw with LVGL and are checked on the PC build's screenshots.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC)/cfg

all: test

apps_hide_test: apps_hide_test.c $(SRC)/cfg/apps_list.c $(SRC)/cfg/apps_list.h
	$(CC) $(CFLAGS) -o $@ apps_hide_test.c $(SRC)/cfg/apps_list.c

test: apps_hide_test
	./apps_hide_test $(SRC)

clean:
	rm -f apps_hide_test

.PHONY: all test clean
