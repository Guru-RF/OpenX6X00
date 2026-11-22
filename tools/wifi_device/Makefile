# Host test of the WiFi device coming and going (src/wifi.cpp): `make` builds it and runs it. The real wifi.cpp on
# the PC build's LVGL (LVGL_LIB, as host/build.sh leaves it in build-host/lib), with GLib's real objects and signals;
# NetworkManager is not asked for anything, and no network of this PC is touched: the client and the devices are the
# test's own objects, and the libnm calls wifi.cpp makes on them are the test's (a symbol of the program goes before
# the library's; the rest of libnm is linked and never called). What wifi.cpp reaches of the rest of the GUI the test
# stands in for; a link error names what it now reaches. Needs libnm's headers and library (pkg-config libnm), as
# the PC build does. See wifi_device_test.cpp for the cases.
TOP      := ../..
SRC      := $(TOP)/src
DEPS     ?= $(TOP)/build-host/deps
LVGL     ?= $(TOP)/lvgl
LVGL_LIB ?= $(TOP)/build-host/lib/liblvgl.a
DEFS     := -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DUSE_SDL=1 -DUSE_DRM=0 -DUSE_FBDEV=0
INC      := -I $(TOP) -I $(SRC) -I $(SRC)/cfg -I $(DEPS)/include -isystem $(LVGL) $(shell pkg-config --cflags libnm)
CXXFLAGS := -std=gnu++17 -O1 -g -pthread $(DEFS) $(INC) -ffunction-sections -fdata-sections
WIFI     ?= $(SRC)/wifi.cpp

all: test

build/wifi.o: $(WIFI) $(wildcard $(SRC)/*.h)
	mkdir -p build && $(CXX) $(CXXFLAGS) -c -o $@ $(WIFI)

# ../speech/silent.c: wifi.cpp says the address aloud (the speech), which is silent here
build/silent.o: ../speech/silent.c
	mkdir -p build && $(CC) -I $(SRC) -c -o $@ ../speech/silent.c

wifi_device_test: wifi_device_test.cpp build/wifi.o build/silent.o $(LVGL_LIB)
	$(CXX) $(CXXFLAGS) -Wall -Wextra -o $@ wifi_device_test.cpp build/wifi.o build/silent.o $(LVGL_LIB) \
	    -Wl,--gc-sections $(shell pkg-config --libs libnm) -lm -pthread

test: wifi_device_test
	./wifi_device_test

clean:
	rm -rf wifi_device_test build

.PHONY: all test clean
