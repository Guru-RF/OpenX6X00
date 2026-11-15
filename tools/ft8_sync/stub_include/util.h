/* Host test shim of src/util.h: the GUI's milliseconds (audio_worker.c, the waterfall's pace) */
#pragma once
#include <stdint.h>
uint64_t get_time(void);
