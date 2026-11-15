/* Host test shim of src/radio.h: what wavelog_cat.c and control_owner.c call. No PTT, no modem: the test does not
 * link if the tune ever keys the radio */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "radio_types.h"
typedef enum { RADIO_RX = 0, RADIO_TX, RADIO_ATU_START, RADIO_ATU_WAIT, RADIO_ATU_RUN, RADIO_SWRSCAN, RADIO_POWEROFF,
               RADIO_OFF } radio_state_t;
radio_state_t radio_get_state(void);
int32_t radio_tx_freq(void);
bool radio_digi_cap(void);
