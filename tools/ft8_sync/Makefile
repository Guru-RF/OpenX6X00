# Host test of the FT8 app's Auto Sync (src/ft8/time_sync.c): `make` builds and runs it. sync_test: the decisions,
# from decodes made up as ft8_window.c gives them (a right clock left alone, one off moved by its error, too few
# stations or slots, a Fox counted once, two camps, the clock set meanwhile, the 4 minutes' window, FT4, FT2 left out,
# the moment in the slot). decoder_test: the times it goes by, with the real decoder (src/ft8/worker.c, ft8_lib) and
# the real decoder thread (src/ft8/audio_worker.c) on FT4 audio in real time, about 40 s. See the two files.
# The decoder's sources are the GUI's, built with their own warnings left off; ft8_lib's headers and libraries are
# host/build.sh's (DEPS, build-host/deps by default). stub_include stands in for LVGL's log and src/util.h's clock in
# audio_worker.c.
TOP    := ../..
SRC    := $(TOP)/src
DEPS   ?= $(TOP)/build-host/deps
DATA   := $(CURDIR)/build/data
INC    := -I stub_include -I $(SRC) -I $(DEPS)/include
CFLAGS := -std=gnu11 -O1 -g -pthread -DHOST_BUILD=1 -DBOARD_DATA='"$(DATA)"' $(INC)
WARN   := -Wall -Wextra
LIBS   := -L $(DEPS)/lib -lft8 -lliquid -lm -Wl,-rpath,$(DEPS)/lib
OURS   := ft8/time_sync.c ft8/utils.c qth/qth.c
GUI    := ft8/worker.c ft8/gfsk.c ft8/hound.c ft8/msg_encode.c ft8/audio_worker.c
HDRS   := $(wildcard $(SRC)/ft8/*.h) $(SRC)/qth/qth.h $(wildcard stub_include/*.h stub_include/*/*.h)

all: test

build/%.o: $(SRC)/%.c $(HDRS)
	mkdir -p $(dir $@) && $(CC) $(CFLAGS) $(if $(filter $(OURS),$*.c),$(WARN)) -c -o $@ $<

sync_test: sync_test.c $(patsubst %.c,build/%.o,$(OURS))
	$(CC) $(CFLAGS) $(WARN) -o $@ $^ -lm

decoder_test: decoder_test.c $(patsubst %.c,build/%.o,$(OURS) $(GUI))
	$(CC) $(CFLAGS) $(WARN) -o $@ $^ $(LIBS)

test: sync_test decoder_test
	mkdir -p $(DATA)
	./sync_test
	./decoder_test

clean:
	rm -rf sync_test decoder_test build

.PHONY: all test clean
