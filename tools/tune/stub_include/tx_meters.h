/* Host test shim of src/tx_meters.h: the readings tx_player steers by, and what the TX views are told */
#pragma once
#include <stdbool.h>
#include <stdint.h>
bool tx_info_refresh(uint8_t *seen, float *alc, float *pwr, float *swr);
void tx_info_set_length(float seconds);
void tx_info_set_tune(float w, float seconds);
