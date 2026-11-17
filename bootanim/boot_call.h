/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The callsign on the boot logo (#292): with Settings > Display > MAIN SCREEN > Callsign on, the callsign of
 *  Settings > General > STATION in the logo's gold over the ring, the last thing to fade in. After the CX8ANM
 *  edition's (condoruy), which drew it there in this font. Only on our logo: an own picture (the web's Boot picture)
 *  is shown as it is.
 *
 *  No framebuffer and no DB here (host test: tools/boot_cw).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BOOT_CALL_BOTTOM    72      /* the ring's first row (tools/logo/mklogo.py): the callsign stays above it */

/* call drawn into logo (w x h, RGB, our logo's layout), centred over the ring. False when nothing was drawn: no
 * callsign, nothing the font has, or too wide for the screen */
bool boot_call_draw(uint8_t *logo, int w, int h, const char *call);
