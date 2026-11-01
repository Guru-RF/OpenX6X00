# Host test of the CW Trainer's answers against the real CW decoder (src/cw_decoder, #144): `make` builds and runs it.
# The rules alone: which characters MorseText marks as the trainer's own (MorseText::own), and the pause between the
# trainer's group and the answer not learned (ElementTimer::turn), the word boundary moving up without it. Then Learn
# and Calls end to end (cw_trainer_test.cpp): the radio keys a group with the sidetone, a user keys it back with the
# keyer 0.3-1.2 s after hearing it, and the decoder hears both through the app's 11025 Hz audio, the resampler and
# the capture's 100 ms fragments, the sound 30-150 ms behind the key, at 10-30 WPM, from a decoder at 20 WPM. Every
# case checks that the radio's own character is never the answer, that no group starts into the answer, that every
# answer comes out right (but one whose first character the decoder ends inside GUARD_MS: the trainer ignores it, as
# before), and that the word boundary stays at 5 dots.
# DEPS: liquid-dsp (host/build.sh puts it in build-host/deps).
SRC      := ../../src
DEPS     ?= ../../build-host/deps
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -g -I $(SRC)/cw_decoder -I $(DEPS)/include
DECODER  := morse_text.cpp tone_finder.cpp tone_lock.cpp key_detector.cpp element_timer.cpp receiver.cpp
HEADERS  := $(wildcard $(SRC)/cw_decoder/*.h)

all: test

cw_trainer_test: cw_trainer_test.cpp $(addprefix $(SRC)/cw_decoder/,$(DECODER) morse_code.c) $(HEADERS)
	mkdir -p build
	$(CC) -std=gnu11 -O2 -c -o build/morse_code.o $(SRC)/cw_decoder/morse_code.c
	$(CXX) $(CXXFLAGS) -o $@ cw_trainer_test.cpp $(addprefix $(SRC)/cw_decoder/,$(DECODER)) build/morse_code.o \
		-L $(DEPS)/lib -Wl,-rpath,$(abspath $(DEPS))/lib -lliquid -lm

test: cw_trainer_test
	./cw_trainer_test

clean:
	rm -rf cw_trainer_test build

.PHONY: all test clean
