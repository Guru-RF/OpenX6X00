# Host test of src/gps.c, the GUI's gpsd client (#237): `make` builds it and runs it, and then s50gpsd_test.sh (the
# ports S50gpsd hands gpsd). gps.c runs on its own thread as on the radio, with the radios' own libgps (gpsd 3.25)
# built for this PC, against a stand-in gpsd on PORT of this PC; see gps_test.c for the cases. Its times are cut
# short, its start of gpsd is a line in test.d/restarts and the GPS ports are under test.d/dev.
# GPSD: the buildtmp folder of a buildroot tree's gpsd 3.25 build, by default the one next to this repository: its
# libgps sources and the files its build generated, gpsd_config.h given this PC's time_t and no D-Bus.
# The sources are copied to build/ first: a quoted #include looks beside the including file before the -I paths, and
# the shims in stub_include must win over src/ (as in tools/beacon); libgps's include "../include/...".
TOP    := ../..
SRC    := $(TOP)/src
GPSD   ?= $(firstword $(wildcard $(TOP)/../AetherX6200Buildroot/build/build/gpsd-3.25/buildtmp \
                                 $(TOP)/../AetherX6100Buildroot/build/build/gpsd-3.25/buildtmp))
PORT   ?= 52947
LIBGPS := ais_json bits gpsdclient gps_maskdump gpsutils hex json libgps_core libgps_json libgps_shm libgps_sock \
          netlib ntpshmread os_compat rtcm2_json rtcm3_json shared_json timespec_str
COPIED := gps.c gps.h usb_devices.h pubsub_ids.h
TIMES  := -DQUIET_MS=400 -DQUIET_MAX_MS=1600 -DRETRY_MAX_MS=2000 -DRESTART_MS=1500 -DRESTART_MAX_MS=3000
CFLAGS := -std=gnu11 -Wall -Wextra -Wno-unused-parameter -O1 -g -pthread -I stub_include -I build/gpsd/include \
          -DHAVE_LIBGPS=1 -DTEST_PORT=$(PORT) -DGPSD_HOST='"127.0.0.1"' -DGPSD_PORT='"$(PORT)"' \
          -DGPS_LINKS='"test.d/dev/gps*"' -DGPSD_RESTART='"echo restart >> test.d/restarts"' $(TIMES)
# time_t is a long on a Linux PC; the radios' is 4 bytes
TIME_T := $(shell echo __SIZEOF_LONG__ | $(CC) -E -P -x c - | tail -1)

all: test

build/libgps.a:
	@test -f "$(GPSD)/libgps/libgps_sock.c" || { echo "GPSD=<buildtmp folder of a gpsd 3.25 buildroot build> needed"; exit 1; }
	rm -rf build/gpsd && mkdir -p build/gpsd/include build/gpsd/libgps
	cp $(GPSD)/include/*.h build/gpsd/include/
	cp $(addprefix $(GPSD)/libgps/,$(addsuffix .c,$(LIBGPS)) ais_json.i) build/gpsd/libgps/
	sed -i -e 's/^#define SIZEOF_TIME_T .*/#define SIZEOF_TIME_T $(TIME_T)/' \
	       -e '/^#define DBUS_EXPORT_ENABLE /d' -e '/^#define HAVE_DBUS /d' build/gpsd/include/gpsd_config.h
	cd build/gpsd/libgps && for f in $(LIBGPS); do $(CC) -c -O1 -w -o $$f.o $$f.c || exit 1; done
	ar rcs $@ build/gpsd/libgps/*.o

build/src/.copied: $(addprefix $(SRC)/,$(COPIED))
	mkdir -p build/src && cp $^ build/src/ && touch $@

gps_test: gps_test.c build/src/.copied build/libgps.a $(wildcard stub_include/*.h stub_include/*/*.h)
	$(CC) $(CFLAGS) -o $@ gps_test.c build/src/gps.c build/libgps.a -lm -Wl,--wrap=gps_open

test: gps_test
	./gps_test
	sh s50gpsd_test.sh $(TOP)/rootfs

clean:
	rm -rf gps_test gps_test.log build test.d

.PHONY: all test clean
