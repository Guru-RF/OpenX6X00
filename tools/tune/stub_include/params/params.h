/* Host test shim of src/params/params.h: the level the digital apps share, the playback gain, the Line-in */
#pragma once
#include <stdint.h>
typedef struct {
    float x;
} params_float_t;
typedef struct {
    params_float_t ft8_output_gain_offset, play_gain_db_f;
    uint8_t        line_in;
} params_t;
extern params_t params;
void params_float_set(params_float_t *p, float v);
