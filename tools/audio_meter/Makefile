# Host test of src/audio_meter.c, the Internet radio's station on the S-meter box (#360): `make` builds it and runs it.
# A piece's level from its samples, the bar's motion, the dB of full scale on the S scale, the queue of levels between
# the playing thread and the meter (on two threads too), and the scale's marks in src/smeter.c and the manual's words
# against that math. See audio_meter_test.c for the cases.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC)

all: test

audio_meter_test: audio_meter_test.c $(SRC)/audio_meter.c $(SRC)/audio_meter.h
	$(CC) $(CFLAGS) -o $@ audio_meter_test.c $(SRC)/audio_meter.c -lm -lpthread

test: audio_meter_test
	./audio_meter_test $(SRC)/smeter.c ../../docs/MANUAL.md

clean:
	rm -f audio_meter_test

.PHONY: all test clean
