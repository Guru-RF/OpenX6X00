# Host tests of the RADE app's End-of-Over callsign: `make` builds and runs them. No radio, no LVGL.
#   freedv_text_test  src/freedv_text.c, the reader of FreeDV's own form, on its own: no RADE library. FreeDV's
#                     sender is checked against what FreeDV's own code sends, then noise, wrong symbols, garbage
#   freedv_text_e2e   src/freedv_rx.c's reading of both forms, then through the RADE modem (rade_c, built for this
#                     PC by tools/rade/build.sh, cached in ~/.cache/openx6x00/rade) and through freedv_rx.c's threads
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O2 -g -DRADE_ENABLED=1 -I $(SRC) -I .
SRCS   := $(SRC)/freedv_text.c $(SRC)/freedv_text.h freedv_text_frames.h

all: test

freedv_text_test: freedv_text_test.c $(SRCS)
	$(CC) $(CFLAGS) -o $@ freedv_text_test.c -lm

# freedv_text.c comes in through the test's #include, for the code table its sender uses
freedv_text_e2e: freedv_text_e2e.c $(SRCS) $(SRC)/freedv_rx.c $(SRC)/freedv_rx.h $(SRC)/olivia_resamp.c ../rade/build.sh
	P=$$(../rade/build.sh) && $(CC) $(CFLAGS) -I $$P/include/rade -I $$P/include/opus -I $$P/include/opus/dnn \
	    -I $$P/include/opus/celt -I $$P/include/opus/include -o $@ freedv_text_e2e.c $(SRC)/freedv_rx.c \
	    $(SRC)/olivia_resamp.c $$P/lib/librade.a -Wl,--whole-archive $$P/lib/libopus.a -Wl,--no-whole-archive \
	    -lm -lpthread

test: freedv_text_test freedv_text_e2e
	./freedv_text_test
	./freedv_text_e2e

clean:
	rm -f freedv_text_test freedv_text_e2e

.PHONY: all test clean
