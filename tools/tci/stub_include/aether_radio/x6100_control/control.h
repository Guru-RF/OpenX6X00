/* Host test shim: the enums of the radio control library the TCI server uses */
#pragma once
typedef enum { x6100_mode_lsb = 0, x6100_mode_lsb_dig, x6100_mode_usb, x6100_mode_usb_dig, x6100_mode_cw,
               x6100_mode_cwr, x6100_mode_am, x6100_mode_nfm } x6100_mode_t;
typedef enum { x6100_agc_off = 0, x6100_agc_slow, x6100_agc_fast, x6100_agc_auto } x6100_agc_t;
