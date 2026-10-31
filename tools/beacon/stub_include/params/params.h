/* Host test shim of src/params/params.h: the beacon's pause and the callsign cw_encoder.c reads */
#pragma once
#include <stdint.h>
typedef struct {
    char x[32];
} params_str_t;
typedef struct {
    uint16_t     cw_encoder_period;
    params_str_t callsign;
} params_t;
extern params_t params;
