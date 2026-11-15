# Host test of the RADE app's FreeDV Reporter (#37): src/freedv_report.c, the GUI's side (the helper's process and
# socket, nothing waits), with a stand-in helper; then rootfs/usr/sbin/x6100-freedv-reporter against the fake
# FreeDV Reporter server of fdvr_test.py. `make` builds and runs both. Nothing goes to qso.freedv.org.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -pthread -I $(SRC) -DBOARD_DATA='"$(CURDIR)/data"'

all: test

freedv_report_test: freedv_report_test.c $(SRC)/freedv_report.c $(SRC)/freedv_report.h
	$(CC) $(CFLAGS) -o $@ freedv_report_test.c $(SRC)/freedv_report.c

test: freedv_report_test
	rm -rf data && mkdir -p data
	./freedv_report_test
	python3 fdvr_test.py

clean:
	rm -rf freedv_report_test data

.PHONY: all test clean
