# Host test of the clock in the field: `make` builds and runs clock_test. The rule (src/clock_rule.c): what keeps the
# clock (a GPS fix, ntpd's word, the kernel's), who may move it (GPS and NTP keep it, a time station moves it and holds
# FT8 off an hour, FT8 only with Auto Sync, Time Sync by hand), ntpd's mode 6 answers made up here, a learned value.
# The time station's minute marker (src/time_station.c) on audio made up here: RWM's carrier keyed with its second
# pulses, WWV's 1000 Hz tone with its ticks, CHU's 500 ms tone among its 300 ms seconds, WWVH's 1200 Hz unheard by a
# WWV listener, noise only, a clock off by a known amount found to the ms, blocks of audio coming late, a restart.
TOP    := ../..
SRC    := $(TOP)/src
CFLAGS := -std=gnu11 -Wall -Wextra -O2 -g -pthread -I $(SRC)
OURS   := $(SRC)/clock_rule.c $(SRC)/time_station.c
HDRS   := $(SRC)/clock_sync.h $(SRC)/time_station.h

all: test

clock_test: clock_test.c $(OURS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ clock_test.c $(OURS) -lm

test: clock_test
	./clock_test

clean:
	rm -f clock_test

.PHONY: all test clean
