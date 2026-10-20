#pragma once
#include <stdbool.h>
#include <stdint.h>
void tx_info_get(float *pwr, float *swr);
bool tx_info_refresh(uint8_t *seen, float *alc, float *pwr, float *swr);
