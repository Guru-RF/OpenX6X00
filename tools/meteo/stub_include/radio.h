/* tools/meteo: the radio's state as meteo_alerts_test.c sets it */
#pragma once
typedef enum { RADIO_RX = 0, RADIO_TX, RADIO_ATU_START, RADIO_ATU_WAIT, RADIO_ATU_RUN, RADIO_SWRSCAN } radio_state_t;
radio_state_t radio_get_state();
