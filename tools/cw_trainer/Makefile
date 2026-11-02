# Host test of the CW Trainer's answers against the real CW decoder (src/cw_decoder, #144): `make` builds and runs it.
# The rules alone: which characters MorseText marks as the trainer's own (MorseText::own), and the pause between the
# trainer's group and the answer not learned (ElementTimer::turn), the word boundary moving up without it, and the
# decoder going down to 5 WPM only while the trainer is open (ElementTimer::trainer_range), and the answer holding one
# place a character, "*" for none (cwt_answer_add in src/cw_trainer_answer.h, which the test shares with the trainer).
# Then Learn and Calls end to end (cw_trainer_test.cpp): the radio keys a group with the sidetone, a user keys it back
# with the keyer 0.3-1.2 s after hearing it, and the decoder hears both through the app's 11025 Hz audio, the resampler
# and the capture's 100 ms fragments, the sound 30-150 ms behind the key, at 10-30 WPM, from a decoder at 20 WPM. Every
# case checks that the radio's own character is never the answer, that no group starts into the answer, that every
# answer comes out right (but one whose first character the decoder ends inside GUARD_MS: the trainer ignores it, as
# before), and that the word boundary stays at 5 dots; also groups of 5 of all 40 at 10 WPM, a straight key at 10 WPM
# answering groups keyed at 14 (the end of a short answer, cwt_answer_ms), groups of 1 and 5 at 5 WPM, answered
# 0.5-1.4 s after them, and at 5 and 6 WPM with KEY Ratio 4.5 and 4 (dahs of 800-1080 ms: the key detector's carrier
# floor, KeyDetector::trainer_range), and groups of 5 at 10 WPM with Ratio 4.5 (a 0 of 26.5 dots, cwt_answer_ms). Then
# groups of 1 and 3 of 3 8 B ? with every ? keyed a dit too many (..--..., no character): never right, the ? heard as
# "*". Last the first 4 groups after the trainer opens, 84 runs of them for each of Learn's groups of 1 and 3 and Calls
# at 10-15 WPM, the sound 30-250 ms behind the key: the lock finds the tone with the first group, and its first dah can
# come in pieces then; the same checks, and the decoder's speed at the end.
# DEPS: liquid-dsp (host/build.sh puts it in build-host/deps).
SRC      := ../../src
DEPS     ?= ../../build-host/deps
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -g -I $(SRC)/cw_decoder -I $(SRC) -I $(DEPS)/include
DECODER  := morse_text.cpp tone_finder.cpp tone_lock.cpp key_detector.cpp element_timer.cpp receiver.cpp
HEADERS  := $(wildcard $(SRC)/cw_decoder/*.h) $(SRC)/cw_trainer_answer.h

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
