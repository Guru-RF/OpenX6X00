/* SPDX-License-Identifier: OFL-1.1
 * pixel_16: 1 bpp, from ter-u16n.bdf of Terminus Font 4.49.1, Copyright (C) 2020 Dimitar Toshkov
 * Zhekov, SIL Open Font License 1.1 (src/fonts/LICENSE-Terminus-OFL-1.1.txt). A Modified Version
 * under the OFL: not "Terminus Font".
 * Converted by tools/fonts/bdf2lvgl.py ter-u16n.bdf pixel_16 pixel_16.c
 */

/*******************************************************************************
 * Size: 16 px
 * Bpp: 1
 * Scale: 1
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0020 */
    /* U+0021 "!" */
    0xfe, 0xc0,
    /* U+0022 """ */
    0x99, 0x90,
    /* U+0023 "#" */
    0x49, 0x24, 0xbf, 0x49, 0x2f, 0xd2, 0x49, 0x20,
    /* U+0024 "$" */
    0x10, 0x21, 0xf4, 0x99, 0x12, 0x1f, 0x09, 0x13, 0x25, 0xf0, 0x81, 0x00,
    /* U+0025 "%" */
    0x65, 0x29, 0xa0, 0x41, 0x02, 0x08, 0x16, 0x52, 0x98,
    /* U+0026 "&" */
    0x31, 0x24, 0x8c, 0x62, 0x58, 0xa2, 0x89, 0xd0,
    /* U+0027 "'" */
    0xe0,
    /* U+0028 "(" */
    0x2a, 0x49, 0x24, 0x44,
    /* U+0029 ")" */
    0x88, 0x92, 0x49, 0x50,
    /* U+002A "*" */
    0x48, 0xcf, 0xcc, 0x48,
    /* U+002B "+" */
    0x21, 0x3e, 0x42, 0x00,
    /* U+002C "," */
    0x58,
    /* U+002D "-" */
    0xfc,
    /* U+002E "." */
    0xc0,
    /* U+002F "/" */
    0x08, 0x44, 0x22, 0x11, 0x08, 0x84, 0x00,
    /* U+0030 "0" */
    0x7a, 0x18, 0x63, 0x96, 0x9c, 0x61, 0x85, 0xe0,
    /* U+0031 "1" */
    0x23, 0x28, 0x42, 0x10, 0x84, 0x27, 0xc0,
    /* U+0032 "2" */
    0x7a, 0x18, 0x41, 0x08, 0x42, 0x10, 0x83, 0xf0,
    /* U+0033 "3" */
    0x7a, 0x18, 0x41, 0x38, 0x10, 0x61, 0x85, 0xe0,
    /* U+0034 "4" */
    0x04, 0x31, 0x49, 0x46, 0x1f, 0xc1, 0x04, 0x10,
    /* U+0035 "5" */
    0xfe, 0x08, 0x20, 0xf8, 0x10, 0x41, 0x85, 0xe0,
    /* U+0036 "6" */
    0x39, 0x08, 0x20, 0xfa, 0x18, 0x61, 0x85, 0xe0,
    /* U+0037 "7" */
    0xfc, 0x10, 0x42, 0x08, 0x41, 0x08, 0x20, 0x80,
    /* U+0038 "8" */
    0x7a, 0x18, 0x61, 0x7a, 0x18, 0x61, 0x85, 0xe0,
    /* U+0039 "9" */
    0x7a, 0x18, 0x61, 0x85, 0xf0, 0x41, 0x09, 0xc0,
    /* U+003A ":" */
    0xc6,
    /* U+003B ";" */
    0x50, 0x16,
    /* U+003C "<" */
    0x08, 0x88, 0x88, 0x20, 0x82, 0x08,
    /* U+003D "=" */
    0xfc, 0x00, 0x3f,
    /* U+003E ">" */
    0x82, 0x08, 0x20, 0x88, 0x88, 0x80,
    /* U+003F "?" */
    0x7a, 0x18, 0x61, 0x08, 0x41, 0x00, 0x10, 0x40,
    /* U+0040 "@" */
    0x7d, 0x06, 0x7d, 0x1a, 0x34, 0x69, 0xcd, 0x80, 0xfc,
    /* U+0041 "A" */
    0x7a, 0x18, 0x61, 0x87, 0xf8, 0x61, 0x86, 0x10,
    /* U+0042 "B" */
    0xfa, 0x18, 0x61, 0xfa, 0x18, 0x61, 0x87, 0xe0,
    /* U+0043 "C" */
    0x7a, 0x18, 0x60, 0x82, 0x08, 0x21, 0x85, 0xe0,
    /* U+0044 "D" */
    0xf2, 0x28, 0x61, 0x86, 0x18, 0x61, 0x8b, 0xc0,
    /* U+0045 "E" */
    0xfe, 0x08, 0x20, 0xf2, 0x08, 0x20, 0x83, 0xf0,
    /* U+0046 "F" */
    0xfe, 0x08, 0x20, 0xf2, 0x08, 0x20, 0x82, 0x00,
    /* U+0047 "G" */
    0x7a, 0x18, 0x60, 0x82, 0x78, 0x61, 0x85, 0xe0,
    /* U+0048 "H" */
    0x86, 0x18, 0x61, 0xfe, 0x18, 0x61, 0x86, 0x10,
    /* U+0049 "I" */
    0xe9, 0x24, 0x92, 0x5c,
    /* U+004A "J" */
    0x1c, 0x20, 0x82, 0x08, 0x20, 0xa2, 0x89, 0xc0,
    /* U+004B "K" */
    0x86, 0x29, 0x28, 0xc3, 0x0a, 0x24, 0x8a, 0x10,
    /* U+004C "L" */
    0x82, 0x08, 0x20, 0x82, 0x08, 0x20, 0x83, 0xf0,
    /* U+004D "M" */
    0x83, 0x8e, 0xac, 0x99, 0x30, 0x60, 0xc1, 0x83, 0x04,
    /* U+004E "N" */
    0x86, 0x18, 0x71, 0xa6, 0x58, 0xe1, 0x86, 0x10,
    /* U+004F "O" */
    0x7a, 0x18, 0x61, 0x86, 0x18, 0x61, 0x85, 0xe0,
    /* U+0050 "P" */
    0xfa, 0x18, 0x61, 0x87, 0xe8, 0x20, 0x82, 0x00,
    /* U+0051 "Q" */
    0x7a, 0x18, 0x61, 0x86, 0x18, 0x61, 0x95, 0xe0, 0x40,
    /* U+0052 "R" */
    0xfa, 0x18, 0x61, 0x87, 0xea, 0x24, 0x8a, 0x10,
    /* U+0053 "S" */
    0x7a, 0x18, 0x20, 0x78, 0x10, 0x61, 0x85, 0xe0,
    /* U+0054 "T" */
    0xfe, 0x20, 0x40, 0x81, 0x02, 0x04, 0x08, 0x10, 0x20,
    /* U+0055 "U" */
    0x86, 0x18, 0x61, 0x86, 0x18, 0x61, 0x85, 0xe0,
    /* U+0056 "V" */
    0x86, 0x18, 0x61, 0x85, 0x24, 0x92, 0x30, 0xc0,
    /* U+0057 "W" */
    0x83, 0x06, 0x0c, 0x18, 0x32, 0x64, 0xd5, 0xc7, 0x04,
    /* U+0058 "X" */
    0x86, 0x14, 0x92, 0x30, 0xc4, 0x92, 0x86, 0x10,
    /* U+0059 "Y" */
    0x83, 0x05, 0x12, 0x22, 0x82, 0x04, 0x08, 0x10, 0x20,
    /* U+005A "Z" */
    0xfc, 0x10, 0x42, 0x10, 0x84, 0x20, 0x83, 0xf0,
    /* U+005B "[" */
    0xf2, 0x49, 0x24, 0x9c,
    /* U+005C "\\" */
    0x84, 0x10, 0x82, 0x10, 0x42, 0x08, 0x40,
    /* U+005D "]" */
    0xe4, 0x92, 0x49, 0x3c,
    /* U+005E "^" */
    0x22, 0xa2,
    /* U+005F "_" */
    0xfc,
    /* U+0060 "`" */
    0x90,
    /* U+0061 "a" */
    0x78, 0x17, 0xe1, 0x86, 0x17, 0xc0,
    /* U+0062 "b" */
    0x82, 0x08, 0x3e, 0x86, 0x18, 0x61, 0x87, 0xe0,
    /* U+0063 "c" */
    0x7a, 0x18, 0x20, 0x82, 0x17, 0x80,
    /* U+0064 "d" */
    0x04, 0x10, 0x5f, 0x86, 0x18, 0x61, 0x85, 0xf0,
    /* U+0065 "e" */
    0x7a, 0x18, 0x7f, 0x82, 0x07, 0x80,
    /* U+0066 "f" */
    0x1c, 0x82, 0x3e, 0x20, 0x82, 0x08, 0x20, 0x80,
    /* U+0067 "g" */
    0x7e, 0x18, 0x61, 0x86, 0x17, 0xc1, 0x05, 0xe0,
    /* U+0068 "h" */
    0x82, 0x08, 0x3e, 0x86, 0x18, 0x61, 0x86, 0x10,
    /* U+0069 "i" */
    0x48, 0x64, 0x92, 0x5c,
    /* U+006A "j" */
    0x08, 0x40, 0x30, 0x84, 0x21, 0x08, 0x63, 0x17, 0x00,
    /* U+006B "k" */
    0x82, 0x08, 0x21, 0x8a, 0x4e, 0x24, 0x8a, 0x10,
    /* U+006C "l" */
    0xc9, 0x24, 0x92, 0x5c,
    /* U+006D "m" */
    0xfd, 0x26, 0x4c, 0x99, 0x32, 0x64, 0x80,
    /* U+006E "n" */
    0xfa, 0x18, 0x61, 0x86, 0x18, 0x40,
    /* U+006F "o" */
    0x7a, 0x18, 0x61, 0x86, 0x17, 0x80,
    /* U+0070 "p" */
    0xfa, 0x18, 0x61, 0x86, 0x1f, 0xa0, 0x82, 0x00,
    /* U+0071 "q" */
    0x7e, 0x18, 0x61, 0x86, 0x17, 0xc1, 0x04, 0x10,
    /* U+0072 "r" */
    0xbf, 0x08, 0x20, 0x82, 0x08, 0x00,
    /* U+0073 "s" */
    0x7e, 0x08, 0x1e, 0x04, 0x1f, 0x80,
    /* U+0074 "t" */
    0x20, 0x82, 0x3e, 0x20, 0x82, 0x08, 0x20, 0x70,
    /* U+0075 "u" */
    0x86, 0x18, 0x61, 0x86, 0x17, 0xc0,
    /* U+0076 "v" */
    0x86, 0x18, 0x52, 0x48, 0xc3, 0x00,
    /* U+0077 "w" */
    0x83, 0x06, 0x4c, 0x99, 0x32, 0x5f, 0x00,
    /* U+0078 "x" */
    0x86, 0x14, 0x8c, 0x4a, 0x18, 0x40,
    /* U+0079 "y" */
    0x86, 0x18, 0x61, 0x86, 0x17, 0xc1, 0x05, 0xe0,
    /* U+007A "z" */
    0xfc, 0x21, 0x08, 0x42, 0x0f, 0xc0,
    /* U+007B "{" */
    0x34, 0x44, 0x84, 0x44, 0x43,
    /* U+007C "|" */
    0xff, 0xc0,
    /* U+007D "}" */
    0xc2, 0x22, 0x12, 0x22, 0x2c,
    /* U+007E "~" */
    0x63, 0x26, 0x30,
    /* U+00A0 */
    /* U+00A1 */
    0xdf, 0xc0,
    /* U+00A2 */
    0x10, 0x21, 0xf4, 0x99, 0x12, 0x24, 0x49, 0x7c, 0x20, 0x40,
    /* U+00A3 */
    0x31, 0x24, 0x10, 0xf1, 0x04, 0x10, 0x47, 0xf0,
    /* U+00A4 */
    0x8b, 0xa3, 0x18, 0xba, 0x20,
    /* U+00A5 */
    0x83, 0x05, 0x11, 0x41, 0x0f, 0x84, 0x3e, 0x10, 0x20,
    /* U+00A6 */
    0xf3, 0xc0,
    /* U+00A7 */
    0x74, 0x60, 0xc9, 0x46, 0x29, 0x30, 0x62, 0xe0,
    /* U+00A8 */
    0x99,
    /* U+00A9 */
    0x7e, 0x81, 0x99, 0xa5, 0xa1, 0xa5, 0x99, 0x81, 0x7e,
    /* U+00AA */
    0x70, 0x5f, 0x17, 0x83, 0xe0,
    /* U+00AB */
    0x12, 0x49, 0x24, 0x84, 0x84, 0x84, 0x80,
    /* U+00AC */
    0xfc, 0x10, 0x41,
    /* U+00AD */
    0xf0,
    /* U+00AE */
    0x7e, 0x81, 0xb9, 0xa5, 0xb9, 0xa9, 0xa5, 0x81, 0x7e,
    /* U+00AF */
    0xf0,
    /* U+00B0 */
    0x69, 0x96,
    /* U+00B1 */
    0x21, 0x3e, 0x42, 0x03, 0xe0,
    /* U+00B2 */
    0x69, 0x12, 0x4f,
    /* U+00B3 */
    0xe1, 0x61, 0x1e,
    /* U+00B4 */
    0x60,
    /* U+00B5 */
    0x86, 0x18, 0x61, 0x86, 0x3f, 0x60, 0x82, 0x00,
    /* U+00B6 */
    0x7f, 0x26, 0x4c, 0x99, 0x2e, 0x44, 0x89, 0x12, 0x24,
    /* U+00B7 */
    0xc0,
    /* U+00B8 */
    0x58,
    /* U+00B9 */
    0x59, 0x25, 0xc0,
    /* U+00BA */
    0x74, 0x63, 0x17, 0x03, 0xe0,
    /* U+00BB */
    0x90, 0x90, 0x90, 0x92, 0x49, 0x24, 0x00,
    /* U+00BC */
    0x20, 0xc0, 0x81, 0x12, 0x41, 0x04, 0x11, 0x47, 0x14, 0x78, 0x10, 0x20,
    /* U+00BD */
    0x20, 0xc0, 0x81, 0x12, 0x41, 0x04, 0x10, 0x4d, 0x24, 0x10, 0x41, 0xe0,
    /* U+00BE */
    0xe0, 0x21, 0x80, 0x9e, 0x41, 0x04, 0x11, 0x47, 0x14, 0x78, 0x10, 0x20,
    /* U+00BF */
    0x20, 0x80, 0x08, 0x21, 0x08, 0x61, 0x85, 0xe0,
    /* U+00C0 */
    0x20, 0x40, 0x1e, 0x86, 0x18, 0x7f, 0x86, 0x18, 0x61,
    /* U+00C1 */
    0x10, 0x80, 0x1e, 0x86, 0x18, 0x7f, 0x86, 0x18, 0x61,
    /* U+00C2 */
    0x31, 0x20, 0x1e, 0x86, 0x18, 0x7f, 0x86, 0x18, 0x61,
    /* U+00C3 */
    0x66, 0x60, 0x1e, 0x86, 0x18, 0x7f, 0x86, 0x18, 0x61,
    /* U+00C4 */
    0x49, 0x20, 0x1e, 0x86, 0x18, 0x7f, 0x86, 0x18, 0x61,
    /* U+00C5 */
    0x31, 0x23, 0x1e, 0x86, 0x18, 0x7f, 0x86, 0x18, 0x61,
    /* U+00C6 */
    0x7f, 0x22, 0x44, 0x8f, 0xd2, 0x24, 0x48, 0x91, 0x3c,
    /* U+00C7 */
    0x7a, 0x18, 0x60, 0x82, 0x08, 0x21, 0x85, 0xe2, 0x08, 0x40,
    /* U+00C8 */
    0x20, 0x40, 0x3f, 0x82, 0x08, 0x3c, 0x82, 0x08, 0x3f,
    /* U+00C9 */
    0x10, 0x80, 0x3f, 0x82, 0x08, 0x3c, 0x82, 0x08, 0x3f,
    /* U+00CA */
    0x31, 0x20, 0x3f, 0x82, 0x08, 0x3c, 0x82, 0x08, 0x3f,
    /* U+00CB */
    0x49, 0x20, 0x3f, 0x82, 0x08, 0x3c, 0x82, 0x08, 0x3f,
    /* U+00CC */
    0x88, 0x74, 0x92, 0x49, 0x70,
    /* U+00CD */
    0x28, 0x74, 0x92, 0x49, 0x70,
    /* U+00CE */
    0x69, 0x0e, 0x44, 0x44, 0x44, 0x4e,
    /* U+00CF */
    0x8c, 0x40, 0xe2, 0x10, 0x84, 0x21, 0x08, 0xe0,
    /* U+00D0 */
    0x78, 0x89, 0x0a, 0x1f, 0x28, 0x50, 0xa1, 0x44, 0xf0,
    /* U+00D1 */
    0x66, 0x60, 0x21, 0x87, 0x1a, 0x65, 0x8e, 0x18, 0x61,
    /* U+00D2 */
    0x20, 0x40, 0x1e, 0x86, 0x18, 0x61, 0x86, 0x18, 0x5e,
    /* U+00D3 */
    0x10, 0x80, 0x1e, 0x86, 0x18, 0x61, 0x86, 0x18, 0x5e,
    /* U+00D4 */
    0x31, 0x20, 0x1e, 0x86, 0x18, 0x61, 0x86, 0x18, 0x5e,
    /* U+00D5 */
    0x66, 0x60, 0x1e, 0x86, 0x18, 0x61, 0x86, 0x18, 0x5e,
    /* U+00D6 */
    0x49, 0x20, 0x1e, 0x86, 0x18, 0x61, 0x86, 0x18, 0x5e,
    /* U+00D7 */
    0x85, 0x23, 0x0c, 0x4a, 0x10,
    /* U+00D8 */
    0x3c, 0x43, 0x42, 0x46, 0x4a, 0x52, 0x62, 0x42, 0xc2, 0x3c,
    /* U+00D9 */
    0x20, 0x48, 0x61, 0x86, 0x18, 0x61, 0x86, 0x18, 0x5e,
    /* U+00DA */
    0x10, 0x88, 0x61, 0x86, 0x18, 0x61, 0x86, 0x18, 0x5e,
    /* U+00DB */
    0x31, 0x20, 0x21, 0x86, 0x18, 0x61, 0x86, 0x18, 0x5e,
    /* U+00DC */
    0x49, 0x20, 0x21, 0x86, 0x18, 0x61, 0x86, 0x18, 0x5e,
    /* U+00DD */
    0x08, 0x22, 0x0c, 0x14, 0x48, 0x8a, 0x08, 0x10, 0x20, 0x40, 0x80,
    /* U+00DE */
    0x82, 0x0f, 0xa1, 0x86, 0x18, 0x7e, 0x82, 0x00,
    /* U+00DF */
    0x72, 0x28, 0xa4, 0xfa, 0x18, 0x61, 0xc6, 0xe0,
    /* U+00E0 */
    0x20, 0x40, 0x1e, 0x05, 0xf8, 0x61, 0x85, 0xf0,
    /* U+00E1 */
    0x10, 0x80, 0x1e, 0x05, 0xf8, 0x61, 0x85, 0xf0,
    /* U+00E2 */
    0x31, 0x20, 0x1e, 0x05, 0xf8, 0x61, 0x85, 0xf0,
    /* U+00E3 */
    0x66, 0x60, 0x1e, 0x05, 0xf8, 0x61, 0x85, 0xf0,
    /* U+00E4 */
    0x49, 0x20, 0x1e, 0x05, 0xf8, 0x61, 0x85, 0xf0,
    /* U+00E5 */
    0x31, 0x23, 0x1e, 0x05, 0xf8, 0x61, 0x85, 0xf0,
    /* U+00E6 */
    0x6c, 0x25, 0xcc, 0xf9, 0x12, 0x1b, 0x00,
    /* U+00E7 */
    0x7a, 0x18, 0x20, 0x82, 0x17, 0x88, 0x21, 0x00,
    /* U+00E8 */
    0x20, 0x40, 0x1e, 0x86, 0x1f, 0xe0, 0x81, 0xe0,
    /* U+00E9 */
    0x10, 0x80, 0x1e, 0x86, 0x1f, 0xe0, 0x81, 0xe0,
    /* U+00EA */
    0x31, 0x20, 0x1e, 0x86, 0x1f, 0xe0, 0x81, 0xe0,
    /* U+00EB */
    0x49, 0x20, 0x1e, 0x86, 0x1f, 0xe0, 0x81, 0xe0,
    /* U+00EC */
    0x88, 0x64, 0x92, 0x5c,
    /* U+00ED */
    0x28, 0x64, 0x92, 0x5c,
    /* U+00EE */
    0x69, 0x06, 0x22, 0x22, 0x27,
    /* U+00EF */
    0x99, 0x06, 0x22, 0x22, 0x27,
    /* U+00F0 */
    0x50, 0x85, 0x1e, 0x86, 0x18, 0x61, 0x85, 0xe0,
    /* U+00F1 */
    0x66, 0x60, 0x3e, 0x86, 0x18, 0x61, 0x86, 0x10,
    /* U+00F2 */
    0x20, 0x40, 0x1e, 0x86, 0x18, 0x61, 0x85, 0xe0,
    /* U+00F3 */
    0x10, 0x80, 0x1e, 0x86, 0x18, 0x61, 0x85, 0xe0,
    /* U+00F4 */
    0x31, 0x20, 0x1e, 0x86, 0x18, 0x61, 0x85, 0xe0,
    /* U+00F5 */
    0x66, 0x60, 0x1e, 0x86, 0x18, 0x61, 0x85, 0xe0,
    /* U+00F6 */
    0x49, 0x20, 0x1e, 0x86, 0x18, 0x61, 0x85, 0xe0,
    /* U+00F7 */
    0x21, 0x01, 0xf0, 0x10, 0x80,
    /* U+00F8 */
    0x02, 0x79, 0x1a, 0x55, 0x2c, 0x50, 0xde,
    /* U+00F9 */
    0x20, 0x40, 0x21, 0x86, 0x18, 0x61, 0x85, 0xf0,
    /* U+00FA */
    0x10, 0x80, 0x21, 0x86, 0x18, 0x61, 0x85, 0xf0,
    /* U+00FB */
    0x31, 0x20, 0x21, 0x86, 0x18, 0x61, 0x85, 0xf0,
    /* U+00FC */
    0x49, 0x20, 0x21, 0x86, 0x18, 0x61, 0x85, 0xf0,
    /* U+00FD */
    0x10, 0x80, 0x21, 0x86, 0x18, 0x61, 0x85, 0xf0, 0x41, 0x78,
    /* U+00FE */
    0x82, 0x08, 0x3e, 0x86, 0x18, 0x61, 0x87, 0xe8, 0x20, 0x80,
    /* U+00FF */
    0x49, 0x20, 0x21, 0x86, 0x18, 0x61, 0x85, 0xf0, 0x41, 0x78,
};

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 128, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 0, .adv_w = 128, .box_w = 1, .box_h = 10, .ofs_x = 3, .ofs_y = 0},
    {.bitmap_index = 2, .adv_w = 128, .box_w = 4, .box_h = 3, .ofs_x = 2, .ofs_y = 8},
    {.bitmap_index = 4, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 12, .adv_w = 128, .box_w = 7, .box_h = 13, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 24, .adv_w = 128, .box_w = 7, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 33, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 41, .adv_w = 128, .box_w = 1, .box_h = 3, .ofs_x = 3, .ofs_y = 8},
    {.bitmap_index = 42, .adv_w = 128, .box_w = 3, .box_h = 10, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 46, .adv_w = 128, .box_w = 3, .box_h = 10, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 50, .adv_w = 128, .box_w = 6, .box_h = 5, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 54, .adv_w = 128, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 58, .adv_w = 128, .box_w = 2, .box_h = 3, .ofs_x = 2, .ofs_y = -1},
    {.bitmap_index = 59, .adv_w = 128, .box_w = 6, .box_h = 1, .ofs_x = 1, .ofs_y = 4},
    {.bitmap_index = 60, .adv_w = 128, .box_w = 1, .box_h = 2, .ofs_x = 3, .ofs_y = 0},
    {.bitmap_index = 61, .adv_w = 128, .box_w = 5, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 68, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 76, .adv_w = 128, .box_w = 5, .box_h = 10, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 83, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 91, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 99, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 107, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 115, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 123, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 131, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 139, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 147, .adv_w = 128, .box_w = 1, .box_h = 7, .ofs_x = 3, .ofs_y = 0},
    {.bitmap_index = 148, .adv_w = 128, .box_w = 2, .box_h = 8, .ofs_x = 2, .ofs_y = -1},
    {.bitmap_index = 150, .adv_w = 128, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 156, .adv_w = 128, .box_w = 6, .box_h = 4, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 159, .adv_w = 128, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 165, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 173, .adv_w = 128, .box_w = 7, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 182, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 190, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 198, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 206, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 214, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 222, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 230, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 238, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 246, .adv_w = 128, .box_w = 3, .box_h = 10, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 250, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 258, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 266, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 274, .adv_w = 128, .box_w = 7, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 283, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 291, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 299, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 307, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 316, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 324, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 332, .adv_w = 128, .box_w = 7, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 341, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 349, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 357, .adv_w = 128, .box_w = 7, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 366, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 374, .adv_w = 128, .box_w = 7, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 383, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 391, .adv_w = 128, .box_w = 3, .box_h = 10, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 395, .adv_w = 128, .box_w = 5, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 402, .adv_w = 128, .box_w = 3, .box_h = 10, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 406, .adv_w = 128, .box_w = 5, .box_h = 3, .ofs_x = 1, .ofs_y = 8},
    {.bitmap_index = 408, .adv_w = 128, .box_w = 6, .box_h = 1, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 409, .adv_w = 128, .box_w = 2, .box_h = 2, .ofs_x = 3, .ofs_y = 10},
    {.bitmap_index = 410, .adv_w = 128, .box_w = 6, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 416, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 424, .adv_w = 128, .box_w = 6, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 430, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 438, .adv_w = 128, .box_w = 6, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 444, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 452, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 460, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 468, .adv_w = 128, .box_w = 3, .box_h = 10, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 472, .adv_w = 128, .box_w = 5, .box_h = 13, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 481, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 489, .adv_w = 128, .box_w = 3, .box_h = 10, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 493, .adv_w = 128, .box_w = 7, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 500, .adv_w = 128, .box_w = 6, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 506, .adv_w = 128, .box_w = 6, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 512, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 520, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 528, .adv_w = 128, .box_w = 6, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 534, .adv_w = 128, .box_w = 6, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 540, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 548, .adv_w = 128, .box_w = 6, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 554, .adv_w = 128, .box_w = 6, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 560, .adv_w = 128, .box_w = 7, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 567, .adv_w = 128, .box_w = 6, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 573, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 581, .adv_w = 128, .box_w = 6, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 587, .adv_w = 128, .box_w = 4, .box_h = 10, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 592, .adv_w = 128, .box_w = 1, .box_h = 10, .ofs_x = 3, .ofs_y = 0},
    {.bitmap_index = 594, .adv_w = 128, .box_w = 4, .box_h = 10, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 599, .adv_w = 128, .box_w = 7, .box_h = 3, .ofs_x = 0, .ofs_y = 8},
    {.bitmap_index = 602, .adv_w = 128, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 602, .adv_w = 128, .box_w = 1, .box_h = 10, .ofs_x = 3, .ofs_y = 0},
    {.bitmap_index = 604, .adv_w = 128, .box_w = 7, .box_h = 11, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 614, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 622, .adv_w = 128, .box_w = 5, .box_h = 7, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 627, .adv_w = 128, .box_w = 7, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 636, .adv_w = 128, .box_w = 1, .box_h = 10, .ofs_x = 3, .ofs_y = 0},
    {.bitmap_index = 638, .adv_w = 128, .box_w = 5, .box_h = 12, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 646, .adv_w = 128, .box_w = 4, .box_h = 2, .ofs_x = 2, .ofs_y = 10},
    {.bitmap_index = 647, .adv_w = 128, .box_w = 8, .box_h = 9, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 656, .adv_w = 128, .box_w = 5, .box_h = 7, .ofs_x = 1, .ofs_y = 4},
    {.bitmap_index = 661, .adv_w = 128, .box_w = 7, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 668, .adv_w = 128, .box_w = 6, .box_h = 4, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 671, .adv_w = 128, .box_w = 4, .box_h = 1, .ofs_x = 2, .ofs_y = 4},
    {.bitmap_index = 672, .adv_w = 128, .box_w = 8, .box_h = 9, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 681, .adv_w = 128, .box_w = 4, .box_h = 1, .ofs_x = 2, .ofs_y = 11},
    {.bitmap_index = 682, .adv_w = 128, .box_w = 4, .box_h = 4, .ofs_x = 2, .ofs_y = 7},
    {.bitmap_index = 684, .adv_w = 128, .box_w = 5, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 689, .adv_w = 128, .box_w = 4, .box_h = 6, .ofs_x = 2, .ofs_y = 5},
    {.bitmap_index = 692, .adv_w = 128, .box_w = 4, .box_h = 6, .ofs_x = 2, .ofs_y = 5},
    {.bitmap_index = 695, .adv_w = 128, .box_w = 2, .box_h = 2, .ofs_x = 3, .ofs_y = 10},
    {.bitmap_index = 696, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 704, .adv_w = 128, .box_w = 7, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 713, .adv_w = 128, .box_w = 1, .box_h = 2, .ofs_x = 3, .ofs_y = 3},
    {.bitmap_index = 714, .adv_w = 128, .box_w = 2, .box_h = 3, .ofs_x = 2, .ofs_y = -3},
    {.bitmap_index = 715, .adv_w = 128, .box_w = 3, .box_h = 6, .ofs_x = 2, .ofs_y = 5},
    {.bitmap_index = 718, .adv_w = 128, .box_w = 5, .box_h = 7, .ofs_x = 1, .ofs_y = 4},
    {.bitmap_index = 723, .adv_w = 128, .box_w = 7, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 730, .adv_w = 128, .box_w = 7, .box_h = 13, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 742, .adv_w = 128, .box_w = 7, .box_h = 13, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 754, .adv_w = 128, .box_w = 7, .box_h = 13, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 766, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 774, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 783, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 792, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 801, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 810, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 819, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 828, .adv_w = 128, .box_w = 7, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 837, .adv_w = 128, .box_w = 6, .box_h = 13, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 847, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 856, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 865, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 874, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 883, .adv_w = 128, .box_w = 3, .box_h = 12, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 888, .adv_w = 128, .box_w = 3, .box_h = 12, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 893, .adv_w = 128, .box_w = 4, .box_h = 12, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 899, .adv_w = 128, .box_w = 5, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 907, .adv_w = 128, .box_w = 7, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 916, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 925, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 934, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 943, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 952, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 961, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 970, .adv_w = 128, .box_w = 6, .box_h = 6, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 975, .adv_w = 128, .box_w = 8, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 985, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 994, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1003, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1012, .adv_w = 128, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1021, .adv_w = 128, .box_w = 7, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1032, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1040, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1048, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1056, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1064, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1072, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1080, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1088, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1096, .adv_w = 128, .box_w = 7, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1103, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 1111, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1119, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1127, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1135, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1143, .adv_w = 128, .box_w = 3, .box_h = 10, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 1147, .adv_w = 128, .box_w = 3, .box_h = 10, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 1151, .adv_w = 128, .box_w = 4, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1156, .adv_w = 128, .box_w = 4, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1161, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1169, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1177, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1185, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1193, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1201, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1209, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1217, .adv_w = 128, .box_w = 5, .box_h = 7, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 1222, .adv_w = 128, .box_w = 7, .box_h = 8, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1229, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1237, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1245, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1253, .adv_w = 128, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1261, .adv_w = 128, .box_w = 6, .box_h = 13, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 1271, .adv_w = 128, .box_w = 6, .box_h = 13, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 1281, .adv_w = 128, .box_w = 6, .box_h = 13, .ofs_x = 1, .ofs_y = -3}
};

static const lv_font_fmt_txt_cmap_t cmaps[] = {
    {.range_start = 32, .range_length = 95, .glyph_id_start = 1, .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY},
    {.range_start = 160, .range_length = 96, .glyph_id_start = 96, .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY}
};

static lv_font_fmt_txt_glyph_cache_t cache;

static const lv_font_fmt_txt_dsc_t font_dsc = {
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 2,
    .bpp = 1,
    .kern_classes = 0,
    .bitmap_format = 0,
    .cache = &cache
};

const lv_font_t pixel_16 = {
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = 16,
    .base_line = 4,
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = -1,
    .underline_thickness = 1,
    .dsc = &font_dsc,
    .fallback = NULL,
    .user_data = NULL,
};
