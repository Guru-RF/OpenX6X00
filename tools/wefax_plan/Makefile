# Host test of WeFax Auto Plan (src/wefax/plan.c) and the quality mark (src/wefax/quality.c): `make` builds and runs
# plan_test. The plan built into the firmware (src/wefax/plan_default.h, made by plan_to_c.py from plan.json) and one
# from the card replacing it; which slot comes next and when, on which frequency, which slot a fax is, a chart going on
# joined, a fax kept against the next slot, the names a fax is saved under and the best copy of a chart (files in
# build/data/wefax), the slot a saved chart came from, the Bulletin's kinds; what comes next of a kind, a chart in
# words, the quality in words and the copy the Bulletin shows; the charts' previews (src/wefax/thumb.c: made, read,
# swept); the mark of charts made up here (clean, noisy, cut short, noise); Auto Tilt (src/wefax/tilt.c) on such a
# chart sheared as a station's line rate shears it, and a station's tilt learned; a chart written with Align and Tilt
# (thumb.c) as the app shows it; Night (src/wefax/night.c with src/sun.c): on, off, by the sun at a locator.
TOP    := ../..
SRC    := $(TOP)/src
DATA   := $(CURDIR)/build/data
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC) -DBOARD_DATA='"$(DATA)"'
OURS   := $(SRC)/wefax/plan.c $(SRC)/wefax/quality.c $(SRC)/wefax/thumb.c $(SRC)/wefax/tilt.c $(SRC)/wefax/night.c \
          $(SRC)/sun.c $(SRC)/qth/qth.c $(SRC)/cJSON.c
HDRS   := $(SRC)/wefax/plan.h $(SRC)/wefax/plan_default.h $(SRC)/wefax/quality.h $(SRC)/wefax/thumb.h \
          $(SRC)/wefax/tilt.h $(SRC)/wefax/night.h $(SRC)/sun.h $(SRC)/qth/qth.h $(SRC)/i18n.h

all: test

# the firmware's plan is plan.json's: a plan.json edited without plan_to_c.py fails here
check_header:
	@python3 -I plan_to_c.py --check

plan_test: plan_test.c $(OURS) $(HDRS)
	$(CC) $(CFLAGS) -pthread -o $@ plan_test.c $(OURS) -lpng -lm

test: check_header plan_test
	rm -rf $(DATA) && mkdir -p $(DATA)/wefax
	TZ=UTC ./plan_test

clean:
	rm -rf plan_test build

.PHONY: all test clean check_header
