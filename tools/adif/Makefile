# Host test of the FT8 app's log, ft_log.adi (src/adif.c), against the day's log the web configurator's Logs page
# lists and exports (src/field/flog.c), issue #174: `make` builds it and runs it. See adif_test.c for the cases.
SRC    := ../../src
# adif.c cuts a longer call or name to the day's log's fields, as the day's log does, and keeps an old helper
CFLAGS := -std=gnu11 -Wall -Wextra -Wno-format-truncation -Wno-unused-function -O1 -g -I $(SRC)

all: test

adif_test: adif_test.c $(SRC)/adif.c $(SRC)/adif.h $(SRC)/field/flog.c $(SRC)/field/flog.h
	$(CC) $(CFLAGS) -o $@ adif_test.c $(SRC)/adif.c $(SRC)/field/flog.c

test: adif_test
	rm -rf build && mkdir -p build
	./adif_test

clean:
	rm -rf adif_test build

.PHONY: all test clean
