# Host test of the boot sound's text (src/boot_cw.c, #289) and of the callsign on the boot logo (bootanim/boot_call.c,
# #292), with the keyer's codes (src/cw_decoder/morse_code.c): `make` builds it and runs it. See boot_cw_test.c.
# `./boot_cw_test --draw in.rgb out.rgb CALL` draws CALL on a logo of 800 x 480 RGB bytes, to look at.
TOP      := ../..
SRC      := $(TOP)/src
BOOT     := $(TOP)/bootanim
CFLAGS   := -std=gnu11 -O1 -g -Wall -Wextra -I $(SRC) -I $(BOOT)
FILES    := $(SRC)/boot_cw.c $(SRC)/cw_decoder/morse_code.c $(BOOT)/boot_call.c
HEADERS  := $(SRC)/boot_cw.h $(SRC)/cw_decoder/morse_code.h $(BOOT)/boot_call.h $(BOOT)/callfont.h

all: test

boot_cw_test: boot_cw_test.c $(FILES) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ boot_cw_test.c $(FILES)

test: boot_cw_test
	./boot_cw_test

clean:
	rm -f boot_cw_test

.PHONY: all test clean
