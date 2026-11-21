# Host test of a page button held (src/button_page.c, #317): `make` builds it and runs it. The real button_page.c
# with button_bar.h's types; button_bar.cpp's page callbacks the test stands in for. LVGL's headers beside src/
# (LVGL) and liquid-dsp's for the GUI's headers (DEPS, as host/build.sh puts them in build-host/deps). See
# page_back_test.c for the cases.
TOP    := ../..
SRC    := $(TOP)/src
DEPS   ?= $(TOP)/build-host/deps
LVGL   ?= $(TOP)/lvgl
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Wno-comment -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE \
          -DLV_LVGL_H_INCLUDE_SIMPLE -I $(TOP) -I $(SRC) -I $(SRC)/cfg -I $(DEPS)/include -isystem $(LVGL)

all: test

page_back_test: page_back_test.c $(SRC)/button_page.c $(SRC)/button_bar.h
	$(CC) $(CFLAGS) -o $@ page_back_test.c $(SRC)/button_page.c

test: page_back_test
	./page_back_test

clean:
	rm -f page_back_test

.PHONY: all test clean
