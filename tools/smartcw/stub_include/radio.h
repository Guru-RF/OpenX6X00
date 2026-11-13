/* Host test shim (tools/smartcw): the key, which the test watches, and the names paddle_keyer.c uses */
#pragma once

#include <stdbool.h>

enum { radio_mode_usb = 2, radio_mode_cw = 4, radio_mode_cwr = 5 };   /* x6100_mode_t */
enum { radio_key_manual = 0, radio_key_auto_left = 1, radio_key_auto_right = 2 };
enum { radio_iambic_a = 0, radio_iambic_b = 1 };

void radio_set_morse_key(bool on);
