# Host test of the Internet radio's station on Bluetooth headphones (src/audio.c: audio_listen_open,
# audio_listen_play), #380: "the speaker took no audio" with a headset, against a model of PulseAudio 16.1's playback
# stream (its tlength, minreq and prebuf on a sink with a fixed latency). The real audio.c and audio_owner.c, built for
# the X6100 and for the X6200 (-DBOARD_X6200=1). `make` builds and runs both; nothing plays.
TOP      := ../..
SRC      := $(TOP)/src
DEPS     ?= $(TOP)/build-host/deps
LVGL     ?= $(TOP)/lvgl
DEFS     := -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DUSE_SDL=1 -DUSE_DRM=0 -DUSE_FBDEV=0
INC      := -I $(TOP) -I $(SRC) -I $(SRC)/cfg -I $(DEPS)/include -isystem $(LVGL)/..
# Only the station's calls are used: the rest of audio.c drops out of the link
GC       := -ffunction-sections -fdata-sections
CFLAGS   := -std=gnu11 -O1 -g -Wall -Wextra $(DEFS) $(INC) $(GC) -Wno-unused-parameter
LDFLAGS  := -Wl,--gc-sections -Wl,--wrap=usleep -lm -lpthread -lasound

all: test

build/x6100/%.o: $(SRC)/%.c $(SRC)/audio.h $(SRC)/audio_owner.h
	mkdir -p build/x6100 && $(CC) $(CFLAGS) -c -o $@ $<

build/x6200/%.o: $(SRC)/%.c $(SRC)/audio.h $(SRC)/audio_owner.h
	mkdir -p build/x6200 && $(CC) $(CFLAGS) -DBOARD_X6200=1 -c -o $@ $<

audio_listen_bt_test: audio_listen_bt_test.c build/x6100/audio.o build/x6100/audio_owner.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

audio_listen_bt_test_x6200: audio_listen_bt_test.c build/x6200/audio.o build/x6200/audio_owner.o
	$(CC) $(CFLAGS) -DBOARD_X6200=1 -o $@ $^ $(LDFLAGS)

test: audio_listen_bt_test audio_listen_bt_test_x6200
	./audio_listen_bt_test
	./audio_listen_bt_test_x6200

clean:
	rm -rf audio_listen_bt_test audio_listen_bt_test_x6200 build

.PHONY: all test clean
