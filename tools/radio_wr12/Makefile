# Host test of radio.c's side of the X6200's WR12 (#134): `make` builds it and runs it. The real src/radio.c, compiled
# as the PC build compiles it for the X6200 (BOARD_X6200, HOST_BUILD), with the real hw_stub.c, base_guard.c,
# ham_bands.c and tx_ranges.c, and stand-ins for the settings, the WR12's driver and the rest of the GUI (fakes.c). What
# radio.c sends the base is caught on its way to hw_stub.c (--wrap); the radio's own thread is not started (--wrap of
# pthread_create): no packets, the base keys by itself only when the test says so, its data is taken as flowing (--wrap
# of base_no_data: these cases are the WR12's; tools/base_guard tests the no-data rule, #152), and the base log as
# free (--wrap of base_guard_log_free: its one line a second is base_guard's), the CW key's pin caught too (--wrap of
# hw_pin_set). radio.c is compiled inside radio_thread.c, which hands the test the radio thread's turn at the base's
# open TX (#176), and its readings of the base's supply (radio_tick, the packets the test's: --wrap of hw_flow_read)
# through the real bat_watch.c to the top bar and the battery model (#223). See radio_wr12_test.c for the cases,
# fakes.h for the stand-ins; radio.c's log goes to radio_wr12_test.log here, removed after a run that passed.
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
          -Wl,--wrap=pthread_detach -Wl,--wrap=base_no_data -Wl,--wrap=base_guard_log_free -Wl,--wrap=hw_pin_set \
          -Wl,--wrap=hw_flow_read

all: test

build/radio.o: radio_thread.c $(SRC)/radio.c $(wildcard $(SRC)/*.h $(SRC)/hw/*.h $(SRC)/cfg/*.h)
	mkdir -p build && $(CC) $(GUI) -c -o $@ radio_thread.c

radio_wr12_test: radio_wr12_test.c fakes.c fakes.h build/radio.o $(SRC)/hw/stub/hw_stub.c $(SRC)/base_guard.c \
                 $(SRC)/pa_bias.c $(SRC)/cfg/ham_bands.c $(SRC)/cfg/tx_ranges.c $(SRC)/bat_watch.c
	$(CC) $(CFLAGS) -o $@ radio_wr12_test.c fakes.c build/radio.o $(SRC)/hw/stub/hw_stub.c $(SRC)/base_guard.c \
	    $(SRC)/pa_bias.c $(SRC)/cfg/ham_bands.c $(SRC)/cfg/tx_ranges.c $(SRC)/bat_watch.c -lm $(WRAPS)

test: radio_wr12_test
	./radio_wr12_test

clean:
	rm -rf radio_wr12_test radio_wr12_test.log build

.PHONY: all test clean
