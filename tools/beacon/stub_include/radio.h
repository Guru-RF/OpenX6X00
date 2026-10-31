/* Host test shim of src/radio.h: what cw_encoder.c calls. beacon_test.c keeps the key and a stand-in base that is in
 * TX while the key is down and for its break-in delay after */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum { RADIO_RX = 0, RADIO_TX, RADIO_ATU_START, RADIO_ATU_WAIT, RADIO_ATU_RUN, RADIO_SWRSCAN, RADIO_POWEROFF,
               RADIO_OFF } radio_state_t;
radio_state_t radio_get_state(void);
void          radio_set_morse_key(bool on);
void          radio_set_qsk_time(uint16_t ms);
void          radio_set_morse_key_limit(uint32_t ms);
unsigned      radio_morse_key_cuts(void);
