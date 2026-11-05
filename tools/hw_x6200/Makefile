# Host test of the X6200's start in src/hw/x6200/hw_x6200.c (#53): `make` builds it and runs it. The PC build has the
# stub radio (hw_stub.c), so hw_x6200.c runs on a PC only here: the real hw_x6200.c and base_guard.c, and the real
# X6200 control library (its src/control.c and src/low/control.c) built here as the shared library the radio has, so
# that hw_x6200.c's x6200_control_cmd sits in front of the library's as it does there. The base is the test's: the I2C
# transfers to it are caught on their way to the kernel (--wrap of open, close, ioctl and usleep, in the library too)
# and kept by address. The library's pins and packet line (gpio.c, flow.c) are left out, stood in by the test.
# X6200_CONTROL: the library's sources, by default those of a buildroot tree built next to this repository
# (buildroot/local-build.sh; v0.1.1, the image's). See hw_x6200_test.c for the cases; the code's own log goes to
# hw_x6200_test.log here, removed after a run that passed.
TOP           := ../..
SRC           := $(TOP)/src
X6200_CONTROL ?= $(firstword $(wildcard $(TOP)/../AetherX6200Buildroot/build/build/aether-x6200-control-v*))
XINC          := -I $(X6200_CONTROL)/include
BUS           := -Wl,--wrap=open -Wl,--wrap=close -Wl,--wrap=ioctl -Wl,--wrap=usleep
# The library and hw_x6200.c as their builds have them, without their warnings; the test's own file with them
LIB           := -std=gnu11 -O1 -g -U_FORTIFY_SOURCE $(XINC)
CFLAGS        := $(LIB) -I $(SRC) -DBOARD_X6200=1 -DBASE_GUARD_LOG='"$(CURDIR)/build/base_i2c.log"'

all: test

build/libaether_x6200_control.so: $(wildcard $(X6200_CONTROL)/src/*.c $(X6200_CONTROL)/src/low/control.c)
	@test -f "$(X6200_CONTROL)/src/low/control.c" || { echo "No X6200 control library sources at '$(X6200_CONTROL)':" \
	    "make X6200_CONTROL=<a buildroot tree>/build/build/aether-x6200-control-v0.1.1"; exit 1; }
	mkdir -p build && $(CC) $(LIB) -fPIC -fvisibility=hidden -shared -o $@ $(X6200_CONTROL)/src/control.c \
	    $(X6200_CONTROL)/src/low/control.c $(BUS)

build/hw_x6200.o: $(SRC)/hw/x6200/hw_x6200.c $(SRC)/hw/hw.h build/libaether_x6200_control.so
	$(CC) $(CFLAGS) -c -o $@ $(SRC)/hw/x6200/hw_x6200.c

hw_x6200_test: hw_x6200_test.c build/hw_x6200.o $(SRC)/base_guard.c build/libaether_x6200_control.so
	$(CC) $(CFLAGS) -Wall -Wextra -o $@ hw_x6200_test.c build/hw_x6200.o $(SRC)/base_guard.c -rdynamic \
	    -L build -laether_x6200_control -Wl,-rpath,'$$ORIGIN/build' -ldl $(BUS)

test: hw_x6200_test
	./hw_x6200_test

clean:
	rm -rf hw_x6200_test hw_x6200_test.log build

.PHONY: all test clean
