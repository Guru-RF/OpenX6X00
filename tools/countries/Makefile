# Host test of src/countries.c, the countries of the first-start guide and Settings > General: `make` builds it with
# the list (src/countries_data.inc, made by gen.py), flags.h's codes and src/itu_region.c, and runs it. The GUI's
# language, the setting, the band plan and cty.dat are stand-ins (countries_test.c). src/countries.c's headers want
# LVGL beside src/ (band_plan.h).
TOP    := ../..
SRC    := $(TOP)/src
LVGL   ?= $(TOP)/lvgl
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE \
          -I $(TOP) -I $(SRC) -I $(SRC)/cfg -isystem $(LVGL)

all: test

countries_test: countries_test.c $(SRC)/countries.c $(SRC)/countries.h $(SRC)/countries_data.inc $(SRC)/flags.h \
                $(SRC)/itu_region.c $(SRC)/itu_region.h
	$(CC) $(CFLAGS) -o $@ countries_test.c $(SRC)/countries.c $(SRC)/itu_region.c -lm

test: countries_test
	./countries_test

clean:
	rm -f countries_test

.PHONY: all test clean
