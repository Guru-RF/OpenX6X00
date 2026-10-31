/* Host test shim of src/util.h: the GUI's milliseconds, on the test's clock */
#pragma once
#include <stdint.h>
uint64_t get_time(void);
