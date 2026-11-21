/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - where the base's frames are centred, Hz from the dial, in receive and in TX (lo_offset.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "lo_offset.h"

#include "../radio_types.h"

int32_t lo_offset_rx(int32_t mode, int32_t key_tone, uint32_t caps) {
    if (caps & HW_CAP_CW_SPECTRUM_ON_DIAL) {
        return 0;
    }
    if (mode == radio_mode_cw) {
        return -key_tone;
    }
    if (mode == radio_mode_cwr) {
        return key_tone;
    }
    return 0;
}

int32_t lo_offset_tx(int32_t mode, int32_t key_tone, uint32_t caps, int32_t zoom) {
    int32_t lo = lo_offset_rx(mode, key_tone, caps);

    if (caps & HW_CAP_TX_SPECTRUM_ON_DIAL) {
        return lo / (zoom > 1 ? zoom : 1);
    }
    return lo;
}
