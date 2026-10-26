/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - country (DXCC entity) of a callsign
 *
 *  From AD1C's cty.dat (country-files.com), the file WSJT-X, JTDX and the
 *  contest loggers use. /mnt/cty.dat, when present, is used instead of the
 *  one in the image (/usr/share/x6100/cty.dat), so a newer list can be put
 *  on the SD card without a new image.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Country of `call`: the entity name, shortened for display where
 * cty.dat's is long ("Germany" for "Fed. Rep. of Germany"), and its
 * primary prefix ("DL"). Either pointer may be NULL. False when the call
 * is unknown, maritime or aeronautical mobile, or no cty.dat was found.
 * The strings stay valid for the life of the program.
 *
 * Thread safe. The file is read on the first call (about 0.1 s), so call
 * it from a worker thread, not from the LVGL thread. */
bool dxcc_lookup(const char *call, const char **name, const char **prefix);

/* The centre of the country of `call` (cty.dat's position), degrees, east
 * and north positive. False when the call is unknown, as above. */
bool dxcc_pos(const char *call, double *lat, double *lon);

/* The DXCC entity of `call`, as its primary prefix ("K", "DL"), NULL when
 * unknown (as dxcc_lookup()). cty.dat's WAE-only entities count as the DXCC
 * entity they are part of: IT9 (Sicily) is "I", GM/s (Shetland) "GM", TA1
 * "TA", 4U1V "OE", IG9 "I", JW/b "JW". Compare the results with strcmp(). */
const char *dxcc_entity(const char *call);

/* The same for a prefix as a person types it ("PD" -> "PA", "OT" -> "ON",
 * "K", "R" -> "UA", "IT9" -> "I"): its longest match in cty.dat's prefix
 * list, single letters included. False when nothing matches. */
bool dxcc_prefix_entity(const char *prefix, const char **entity);

#ifdef __cplusplus
}
#endif
