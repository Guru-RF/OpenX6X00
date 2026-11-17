# Host test of the boot sound's text (src/boot_cw.c, #289) with the keyer's codes (src/cw_decoder/morse_code.c): `make`
# builds it and runs it. See boot_cw_test.c.
TOP      := ../..
SRC      := $(TOP)/src
CFLAGS   := -std=gnu11 -O1 -g -Wall -Wextra -I $(SRC)
FILES    := $(SRC)/boot_cw.c $(SRC)/cw_decoder/morse_code.c
HEADERS  := $(SRC)/boot_cw.h $(SRC)/cw_decoder/morse_code.h

all: test

boot_cw_test: boot_cw_test.c $(FILES) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ boot_cw_test.c $(FILES)

test: boot_cw_test
	./boot_cw_test

clean:
	rm -f boot_cw_test

.PHONY: all test clean
