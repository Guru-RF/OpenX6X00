/* Host test shim of src/radio.h: what src/winlink.c calls */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum { RADIO_RX = 0, RADIO_TX } radio_state_t;
enum { radio_mode_lsb = 1, radio_mode_usb = 2, radio_mode_usb_dig = 3 };
radio_state_t radio_get_state(void);
bool     radio_get_modem(void);
int32_t  radio_tx_freq(void);
unsigned radio_swr_trips(void);
uint32_t radio_caps(void);
#define HW_CAP_TX_FILTER (1 << 10)
