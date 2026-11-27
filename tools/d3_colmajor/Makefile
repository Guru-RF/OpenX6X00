# Host test of the 3D waterfall's landscape picture (d3_render() in src/waterfall_view.c): `make` builds it and runs
# it. A waterfall without planes (the PC build, a panel on fbdev, a single DRM plane) draws the 3D picture into
# columns of its own and turns them over into the image; with the planes it is drawn in the plane's own columns and
# never turned. The test renders 270 pictures drawn in rows, as before, and the same 270 drawn in columns and
# turned, and compares them pixel for pixel. The renderer is taken out of src/waterfall_view.c as it is (awk, from
# "#define D3_STEP" to the labels); see d3_colmajor_test.c for the cases and what stands in for the rest.
#   make bench    the time of a picture each way, on this PC (its caches are not a Cortex-A7's: the turn is for
#                 the radio, where a landscape column's pixels, 3200 bytes apart, each miss the cache)
#   make arm CROSS=<buildroot>/host/bin/arm-buildroot-linux-gnueabihf- [QEMU=qemu-arm-static]
#                 the same test with the radio's compiler and flags (CMakeLists.txt), so the NEON turn: run under
#                 QEMU if given, else d3_colmajor_test.arm is left to run on a radio (OUT rows|turn [bench], then
#                 cmp the two OUTs)
# BOARD_DEFS as cmake gives them (-DBOARD_X6200=1): the panel's size is board.h's.
TOP      := ../..
SRC      := $(TOP)/src
CFLAGS   := -std=gnu11 -O2 -g -Wall -Wno-unused-function -Wno-unused-variable -I build -I $(SRC) $(BOARD_DEFS)
ARMFLAGS := -std=gnu11 -O3 -mtune=cortex-a7 -mcpu=cortex-a7 -mfloat-abi=hard -mfpu=neon-vfpv4 -static \
            -Wall -Wno-unused-function -Wno-unused-variable -I build -I $(SRC) $(BOARD_DEFS)

all: test

build/d3_extract.inc: $(SRC)/waterfall_view.c
	mkdir -p build
	awk '/^#define D3_STEP/{p=1} /^\/\* The reference frequencies under the front edge/{p=0} p' $< > $@
	grep -q 'static void d3_transpose' $@ && grep -q 'static void d3_render' $@

d3_colmajor_test: d3_colmajor_test.c build/d3_extract.inc $(SRC)/board.h $(SRC)/wf3d_shape.c
	$(CC) $(CFLAGS) -o $@ d3_colmajor_test.c $(SRC)/wf3d_shape.c -lm

test: d3_colmajor_test
	./d3_colmajor_test build/rows.sums rows
	./d3_colmajor_test build/turn.sums turn
	cmp build/rows.sums build/turn.sums
	@echo "identical: $$(wc -l < build/turn.sums) pictures"

bench: d3_colmajor_test
	./d3_colmajor_test /dev/null rows bench
	./d3_colmajor_test /dev/null turn bench

arm: d3_colmajor_test.c build/d3_extract.inc $(SRC)/board.h $(SRC)/wf3d_shape.c
	$(CROSS)gcc $(ARMFLAGS) -o d3_colmajor_test.arm d3_colmajor_test.c $(SRC)/wf3d_shape.c -lm
ifneq ($(QEMU),)
	$(QEMU) ./d3_colmajor_test.arm build/rows.arm.sums rows
	$(QEMU) ./d3_colmajor_test.arm build/turn.arm.sums turn
	cmp build/rows.arm.sums build/turn.arm.sums
	@echo "identical: $$(wc -l < build/turn.arm.sums) pictures (NEON turn)"
endif

clean:
	rm -rf d3_colmajor_test d3_colmajor_test.arm build

.PHONY: all test bench arm clean
