# Host test of how the FT8 app's list draws its rows (src/ft8/row_look.c): CQ rows Edge, F4OLI's redesign and the
# default, and Filled, the green rows of v0.3.12 (#359). `make` builds and runs it. row_look.h wants LVGL's headers
# beside src/ (theme.h, decode_list.h), nothing of LVGL is linked.
TOP    := ../..
SRC    := $(TOP)/src
LVGL   ?= $(TOP)/lvgl
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE \
          -I $(TOP) -I $(SRC) -isystem $(LVGL)

all: test

ft8_rows_test: ft8_rows_test.c $(SRC)/ft8/row_look.c $(SRC)/ft8/row_look.h $(SRC)/ft8/decode_list.h $(SRC)/theme.h
	$(CC) $(CFLAGS) -o $@ ft8_rows_test.c $(SRC)/ft8/row_look.c

test: ft8_rows_test
	./ft8_rows_test

clean:
	rm -f ft8_rows_test

.PHONY: all test clean
