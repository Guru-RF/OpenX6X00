# Host test of dsp_db_to_pow() and dsp_pow_to_db() (src/dsp_math.h), the dB <-> power of every bin of the panorama:
# `make` builds it and runs it. Their error next to powf's and log10f's, that the waterfall's colours, the spectrum's
# pixels and the noise floor stay where they were, and the time per bin of both. -O2: the time is of optimized loops,
# the radio's build has -O3. See dsp_math_test.cpp.
# `make arm CROSS=<buildroot>/host/bin/arm-buildroot-linux-gnueabihf-` builds dsp_math_test.arm with the radio's
# compiler and flags (CMakeLists.txt); on the radio `./dsp_math_test.arm bench` times it on the Cortex-A7.
SRC      := ../../src
CXXFLAGS := -std=gnu++17 -O2 -g -Wall -Wextra -I $(SRC)
ARMFLAGS := -std=gnu++17 -O3 -mtune=cortex-a7 -mcpu=cortex-a7 -mfloat-abi=hard -mfpu=neon-vfpv4 -I $(SRC)

all: test

dsp_math_test: dsp_math_test.cpp $(SRC)/dsp_math.h
	$(CXX) $(CXXFLAGS) -o $@ dsp_math_test.cpp -lm

test: dsp_math_test
	./dsp_math_test

arm: dsp_math_test.cpp $(SRC)/dsp_math.h
	$(CROSS)g++ $(ARMFLAGS) -o dsp_math_test.arm dsp_math_test.cpp -lm

clean:
	rm -f dsp_math_test dsp_math_test.arm

.PHONY: all test arm clean
