/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - where the base's frames are centred, Hz from the dial, in receive and in TX: the offsets the waterfall
 *  and the spectrum place them by (cp_mode_lo_offset and cp_mode_tx_lo_offset of the settings). tools/lo_offset checks
 *  them, the TX one against the carrier as the reporter of #191 measured it on Xiegu's base 1.1.9.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* In receive. The X6100's in CW and CW-R on the oscillator the tone is heard from, the key tone below the dial in CW
 * and above it in CW-R (the patched base's lo_freq); 0 in the other modes, and in every mode with
 * HW_CAP_CW_SPECTRUM_ON_DIAL, the X6200's, where this offset drew a station on the dial a key tone off it (#115).
 * mode: a radio_mode_t; caps: the base's HW_CAP_* bits */
int32_t lo_offset_rx(int32_t mode, int32_t key_tone, uint32_t caps);

/* While the base transmits, at the zoom the picture is drawn at: the receive offset (Xiegu's 1.1.8 and older, the
 * patched bases, the X6200). Xiegu's X6100 base from 1.1.9 on (HW_CAP_TX_SPECTRUM_ON_DIAL): the receive offset over
 * the zoom. There the TX frames come as if unzoomed, centred on the oscillator as 1.1.8's: the carrier is a key tone
 * from their middle, and drawn over the zoom's span, a key tone over the zoom from it. Placed on the dial (v0.3.11 to
 * v0.3.13) the carrier was drawn that far off the line, above it in CW and below it in CW-R: at zoom 8 the reporter
 * measured an eighth of the key tone from 500 to 1200 Hz, with an IF shift of -3000 Hz; placed as in receive (up to
 * v0.3.10), seven eighths of it below the line in CW (#191) */
int32_t lo_offset_tx(int32_t mode, int32_t key_tone, uint32_t caps, int32_t zoom);

#ifdef __cplusplus
}
#endif
