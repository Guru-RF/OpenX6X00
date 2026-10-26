/* Host test shim of src/radio.h: what the TCI server and control_owner.c call */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "radio_types.h"
typedef enum { RADIO_RX = 0, RADIO_TX, RADIO_ATU_START, RADIO_ATU_WAIT, RADIO_ATU_RUN, RADIO_SWRSCAN, RADIO_POWEROFF,
               RADIO_OFF } radio_state_t;
radio_state_t radio_get_state(void);
void radio_set_ptt(bool tx);
void radio_set_modem(bool tx);
bool radio_get_modem(void);
void radio_set_pwr(float d);
void radio_set_tci_mute(bool on);
uint32_t radio_caps(void);
