# Host test of the FT8/FT4/FT2 QSO sequence (src/ft8/qso.cpp, #150): `make` builds and runs it, `make verbose` prints
# each scenario's messages too. The real qso.cpp, with src/ft8/utils.c and src/qth/qth.c as on the radio, gets each
# slot's decodes, the slot's end and the operator's presses (a row, TX CQ, Force QSO save, Hound) as ft8_window.c
# gives them; what we send counts its repeats as transmit() does, and goes out in the slots between the other
# station's. The good paths (we call CQ, we answer one, RRR, a 73 for an RR73 missed, RR73 in a multi-stream message,
# other stations in between, Auto off, a POTA caller first), and #150's: the operator moves on (another station, TX CQ)
# or the retries run out before the station's last message comes, and a new caller once our RR73 or 73 went. Each
# checks what is logged: once, with the right reports, never a QSO that was not complete.
# qso.cpp's own warnings under -Wextra (a switch without FXT_MSG_TYPE_OTHER, an snr unused, the R+report case falling
# through to the report's on purpose) are left off.
SRC      := ../../src
CFLAGS   := -std=gnu11 -Wall -Wextra -O1 -g
CXXFLAGS := -std=c++17 -Wall -Wextra -Wno-switch -Wno-unused-parameter -Wno-implicit-fallthrough -O1 -g -I $(SRC)/ft8
SOURCES  := $(SRC)/ft8/qso.cpp $(SRC)/ft8/qso.h $(SRC)/ft8/utils.c $(SRC)/ft8/utils.h $(SRC)/qth/qth.c $(SRC)/qth/qth.h

all: test

qso_test: qso_test.cpp $(SOURCES)
	mkdir -p build
	$(CC) $(CFLAGS) -c -o build/utils.o $(SRC)/ft8/utils.c
	$(CC) $(CFLAGS) -c -o build/qth.o $(SRC)/qth/qth.c
	$(CXX) $(CXXFLAGS) -o $@ qso_test.cpp $(SRC)/ft8/qso.cpp build/utils.o build/qth.o -lm

test: qso_test
	./qso_test

verbose: qso_test
	./qso_test -v

clean:
	rm -rf qso_test build

.PHONY: all test verbose clean
