# Host test of who has the speaker (src/audio_owner.c): the claims of the listeners (the Internet radio's station, DMR)
# and the voice links (Zello, SVXConnect) counted into the speaker's play mode and the headphones' receiver mute, and a
# voice link's message over a listener. `make` builds and runs it; nothing plays.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -Werror -O1 -g -I $(SRC)

all: test

audio_owner_test: audio_owner_test.c $(SRC)/audio_owner.c $(SRC)/audio_owner.h $(SRC)/audio.h
	$(CC) $(CFLAGS) -o $@ audio_owner_test.c $(SRC)/audio_owner.c

test: audio_owner_test
	./audio_owner_test

clean:
	rm -f audio_owner_test

.PHONY: all test clean
