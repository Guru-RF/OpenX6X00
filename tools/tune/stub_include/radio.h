/* Host test shim of src/radio.h: what tx_player.c and tune_tx.c call. tune_test.c keeps a stand-in base that these
 * set as radio.c sets the real one */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "radio_types.h"
typedef enum { RADIO_RX = 0, RADIO_TX, RADIO_ATU_START, RADIO_ATU_WAIT, RADIO_ATU_RUN, RADIO_SWRSCAN, RADIO_POWEROFF,
               RADIO_OFF } radio_state_t;
radio_state_t radio_get_state(void);
bool          radio_get_modem(void);
void          radio_set_modem(bool tx);
int32_t       radio_tx_freq(void);
void          radio_set_tx_freq(int32_t freq);
void          radio_set_pwr(float w);
uint32_t      radio_caps(void);
void          radio_set_tune(float w, uint32_t limit_ms);
void          radio_set_tx_mode(int mode);
