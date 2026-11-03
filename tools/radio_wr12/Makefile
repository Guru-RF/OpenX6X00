# Host test of radio.c's side of the X6200's WR12 (#134): `make` builds it and runs it. The real src/radio.c, compiled
# as the PC build compiles it for the X6200 (BOARD_X6200, HOST_BUILD), with the real hw_stub.c, base_guard.c and
# ham_bands.c, and stand-ins for the settings, the WR12's driver and the rest of the GUI (fakes.c). What radio.c
# sends the base is caught on its way to hw_stub.c (--wrap); the radio's own thread is not started (--wrap of
# pthread_create): no packets, the base never keys by itself. See radio_wr12_test.c for the cases, fakes.h for the
# stand-ins; radio.c's log goes to radio_wr12_test.log here, removed after a run that passed.
# radio.c's headers want what the PC build has (host/README.md): LVGL beside src/ (LVGL), liquid-dsp's headers
# (DEPS, as host/build.sh puts them in build-host/deps), and libnm's (pkg-config).
TOP    := ../..
SRC    := $(TOP)/src
DEPS   ?= $(TOP)/build-host/deps
LVGL   ?= $(TOP)/lvgl
DEFS   := -DBOARD_X6200=1 -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DUSE_SDL=1 -DUSE_DRM=0 \
          -DUSE_FBDEV=0 -DBOARD_DATA='"$(CURDIR)/build"' -DBOARD_SHARE='"$(CURDIR)/build"' \
          -DBASE_GUARD_LOG='"$(CURDIR)/build/base_i2c.log"'
INC    := -I $(TOP) -I $(SRC) -I $(SRC)/cfg -I $(DEPS)/include -isystem $(LVGL) $(shell pkg-config --cflags libnm)
# radio.c as the PC build has it, without its warnings; the test's own files with them (but params/types.h's comment)
GUI    := -std=gnu11 -O1 -g -pthread $(DEFS) $(INC)
CFLAGS := $(GUI) -Wall -Wextra -Wno-comment
WRAPS  := -Wl,--wrap=hw_vfo_set_freq -Wl,--wrap=hw_set_i -Wl,--wrap=hw_set_f -Wl,--wrap=pthread_create \
          -Wl,--wrap=pthread_detach

all: test

build/radio.o: $(SRC)/radio.c $(wildcard $(SRC)/*.h $(SRC)/hw/*.h $(SRC)/cfg/*.h)
	mkdir -p build && $(CC) $(GUI) -c -o $@ $(SRC)/radio.c

radio_wr12_test: radio_wr12_test.c fakes.c fakes.h build/radio.o $(SRC)/hw/stub/hw_stub.c $(SRC)/base_guard.c \
                 $(SRC)/cfg/ham_bands.c
	$(CC) $(CFLAGS) -o $@ radio_wr12_test.c fakes.c build/radio.o $(SRC)/hw/stub/hw_stub.c $(SRC)/base_guard.c \
	    $(SRC)/cfg/ham_bands.c -lm $(WRAPS)

test: radio_wr12_test
	./radio_wr12_test

clean:
	rm -rf radio_wr12_test radio_wr12_test.log build

.PHONY: all test clean
