# Host test of the Zello app (APP > Zello): `make` builds and runs the three, nothing contacts Zello.
# - zello_ptt_test: src/zello_ptt.c, the radio's PTT key never keys the radio while the app is open and keys it again
#   at once when the app has closed; Zello's audio path the radio's own while it transmits and half a second after.
# - zello_link_test: src/zello_link.c, the Simplex repeater's rules: it keys only when they let it, the time limit,
#   the operator's PTT first, nothing to Zello while the radio transmits (no loop), the CW ID's timing; the squelch
#   as the audio shows it, the ID's Morse.
# - zello_test.py: rootfs/usr/sbin/x6100-zello, the helper, with the saved samples of samples/ (mksamples.py) and a
#   stand-in Channel API server on 127.0.0.1; any other address fails the test. Opus through the PC's libopus.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -Werror -O1 -g -I $(SRC)

all: test

zello_ptt_test: zello_ptt_test.c $(SRC)/zello_ptt.c $(SRC)/zello_ptt.h
	$(CC) $(CFLAGS) -o $@ zello_ptt_test.c $(SRC)/zello_ptt.c

zello_link_test: zello_link_test.c $(SRC)/zello_link.c $(SRC)/zello_link.h
	$(CC) $(CFLAGS) -o $@ zello_link_test.c $(SRC)/zello_link.c -lm

test: zello_ptt_test zello_link_test
	./zello_ptt_test
	./zello_link_test
	python3 -I -B zello_test.py

clean:
	rm -f zello_ptt_test zello_link_test

.PHONY: all test clean
