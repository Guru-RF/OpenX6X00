# Host test of where the filter shade goes (src/filter_shade.h, #250): `make` builds it and runs it. The header alone,
# as the spectrum (spectrum_view.c) and the waterfall (waterfall_view.c) take it: the columns shaded with Passband and
# Outside, cut to the view, and the columns the waterfall draws again when the passband moves. See filter_shade_test.c.
TOP      := ../..
SRC      := $(TOP)/src
CFLAGS   := -std=gnu11 -O1 -g -Wall -Wextra -I $(SRC)

all: test

filter_shade_test: filter_shade_test.c $(SRC)/filter_shade.h
	$(CC) $(CFLAGS) -o $@ filter_shade_test.c

test: filter_shade_test
	./filter_shade_test

clean:
	rm -f filter_shade_test

.PHONY: all test clean
