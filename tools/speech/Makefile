# Host tests of the spoken feedback: the synthesizer's loading and cutting (src/speech_engine.c) and the rules of what
# is said and when (src/voice.cpp, the GUI stood in for by stubs/). `make` builds and runs them without RHVoice, as on
# a card image without speech: nothing speaks, nothing waits, the words are made right. With a libRHVoice built for
# this PC they speak too (nothing plays: the samples are counted, at the pace they would have played):
#   make SPEECH_LIB=<build>/src/lib/libRHVoice.so SPEECH_DATA=<RHVoice>/data SPEECH_CONFIG=<RHVoice>/config
# (RHVoice 1.14.0 as the card image has it: cmake -S <RHVoice> -B <build> && cmake --build <build> --target RHVoice)
SRC      := ../../src
RHV      := ../../third-party/rhvoice
CFLAGS   := -std=gnu11 -Wall -Wextra -Werror -O1 -g -I $(SRC) -I $(RHV)
CXXFLAGS := -std=gnu++17 -Wall -Wextra -Werror -O1 -g -I build -I stubs -I $(SRC) -I $(RHV)
LIBS     := -ldl -lpthread $(shell pkg-config --libs libpulse-simple)
ENV      := SPEECH_LIB=$(SPEECH_LIB) SPEECH_DATA=$(SPEECH_DATA) SPEECH_CONFIG=$(SPEECH_CONFIG)

all: test

speech_engine_test: speech_engine_test.c $(SRC)/speech_engine.c $(SRC)/speech_engine.h
	$(CC) $(CFLAGS) -o $@ speech_engine_test.c $(SRC)/speech_engine.c $(LIBS)

# voice.cpp is compiled from a copy in build/: next to the GUI's own headers it would take those, not the stubs
build/voice.cpp: $(SRC)/voice.cpp
	mkdir -p build && cp $< $@

build/speech_engine.o: $(SRC)/speech_engine.c $(SRC)/speech_engine.h
	mkdir -p build && $(CC) $(CFLAGS) -c -o $@ $<

voice_test: voice_test.cpp build/voice.cpp build/speech_engine.o $(SRC)/voice.h stubs/stubs.h
	$(CXX) $(CXXFLAGS) -o $@ voice_test.cpp build/voice.cpp build/speech_engine.o $(LIBS)

test: speech_engine_test voice_test
	$(ENV) ./speech_engine_test
	$(ENV) ./voice_test

clean:
	rm -rf speech_engine_test voice_test build

.PHONY: all test clean
