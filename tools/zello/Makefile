# Host test of the Zello app (APP > Zello): `make` builds and runs the three, nothing contacts Zello.
# - zello_ptt_test: src/voice_ptt.c (shared with SVXConnect), the radio's PTT key never keys the radio while the app
#   is open and keys it again at once when the app has closed; Zello's audio path the radio's own while it transmits
#   and half a second after.
# - zello_link_test: src/voice_link.c (shared with SVXConnect), the Simplex repeater's rules: it keys only when they
#   let it and only inside the amateur bands, the time limit, the operator's PTT first, nothing to Zello while the
#   radio transmits (no loop) nor from outside the amateur bands, the CW ID's timing; the squelch as the audio shows
#   it, the ID's Morse.
# - zello_test.py: rootfs/usr/sbin/x6100-zello, the helper, with the saved samples of samples/ (mksamples.py) and a
#   stand-in Channel API server on 127.0.0.1; any other address fails the test. Opus through the PC's libopus.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -Werror -O1 -g -I $(SRC)

all: test

zello_ptt_test: zello_ptt_test.c $(SRC)/voice_ptt.c $(SRC)/voice_ptt.h
	$(CC) $(CFLAGS) -o $@ zello_ptt_test.c $(SRC)/voice_ptt.c

zello_link_test: zello_link_test.c $(SRC)/voice_link.c $(SRC)/voice_link.h $(SRC)/cfg/ham_bands.c
	$(CC) $(CFLAGS) -o $@ zello_link_test.c $(SRC)/voice_link.c $(SRC)/cfg/ham_bands.c -lm

test: zello_ptt_test zello_link_test
	./zello_ptt_test
	./zello_link_test
	python3 -I -B zello_test.py

clean:
	rm -f zello_ptt_test zello_link_test

.PHONY: all test clean
