# Host test of the FT8 app's tiles and lane (src/ft8/heard.c, F4OLI's redesign) and of the time a slot's rows and our
# TX row carry (ftx_slot_start, src/ft8/utils.c): `make` builds and runs it. heard.h wants LVGL's headers beside src/
# (decode_list.h), nothing of LVGL is linked; utils.c wants qth.c (its grid check).
TOP    := ../..
SRC    := $(TOP)/src
LVGL   ?= $(TOP)/lvgl
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE \
          -I $(TOP) -I $(SRC) -isystem $(LVGL)

all: test

ft8_tiles_test: ft8_tiles_test.c $(SRC)/ft8/heard.c $(SRC)/ft8/heard.h $(SRC)/ft8/decode_list.h $(SRC)/ft8/utils.c \
                $(SRC)/ft8/utils.h $(SRC)/qth/qth.c
	$(CC) $(CFLAGS) -o $@ ft8_tiles_test.c $(SRC)/ft8/heard.c $(SRC)/ft8/utils.c $(SRC)/qth/qth.c -lm

test: ft8_tiles_test
	./ft8_tiles_test

clean:
	rm -f ft8_tiles_test

.PHONY: all test clean
