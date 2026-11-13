# Host test of src/cpu_power.c: a radio that v0.3.11's Saves power left on ondemand put back to full speed, on a
# folder laid out as the radios' cpufreq files (test.d, made and removed by the test), and what is left alone. `make`
# builds it and runs it. See cpu_power_test.c for the cases.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -O1 -g -I $(SRC) -DCPUFREQ_DIR='"test.d/cpufreq"'

all: test

cpu_power_test: cpu_power_test.c $(SRC)/cpu_power.c $(SRC)/cpu_power.h
	$(CC) $(CFLAGS) -o $@ cpu_power_test.c $(SRC)/cpu_power.c

test: cpu_power_test
	./cpu_power_test

clean:
	rm -rf cpu_power_test test.d

.PHONY: all test clean
