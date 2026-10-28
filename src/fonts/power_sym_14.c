/*******************************************************************************
 * Size: 14 px
 * Bpp: 4
 * Opts: --bpp 4 --size 14 --no-compress --no-prefilter --font lvgl/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff --range 0xF1E6 --format lvgl --lv-include lvgl/lvgl.h -o src/fonts/power_sym_14.c
 * Then by hand: the line height and baseline of lv_font_montserrat_14, and that font as the
 * fallback, so a label in this font shows the plug and every other symbol and character as before.
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#ifndef POWER_SYM_14
#define POWER_SYM_14 1
#endif

#if POWER_SYM_14

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+F1E6 "" */
    0x0, 0x10, 0x0, 0x1, 0x0, 0x0, 0x2f, 0x60,
    0x0, 0xea, 0x0, 0x4, 0xf8, 0x0, 0xf, 0xc0,
    0x0, 0x4f, 0x80, 0x0, 0xfc, 0x0, 0x3, 0xc6,
    0x0, 0xc, 0x90, 0xb, 0xcc, 0xcc, 0xcc, 0xcc,
    0xc5, 0xef, 0xff, 0xff, 0xff, 0xff, 0x61, 0xff,
    0xff, 0xff, 0xff, 0x90, 0xf, 0xff, 0xff, 0xff,
    0xf8, 0x0, 0xbf, 0xff, 0xff, 0xff, 0x30, 0x3,
    0xff, 0xff, 0xff, 0xa0, 0x0, 0x3, 0xcf, 0xff,
    0x80, 0x0, 0x0, 0x0, 0xaf, 0x20, 0x0, 0x0,
    0x0, 0xa, 0xf2, 0x0, 0x0, 0x0, 0x0, 0x8d,
    0x10, 0x0, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 168, .box_w = 11, .box_h = 15, .ofs_x = 0, .ofs_y = -2}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 61926, .range_length = 1, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LV_VERSION_CHECK(8, 0, 0)
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LV_VERSION_CHECK(8, 0, 0)
    .cache = &cache
#endif
};


/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LV_VERSION_CHECK(8, 0, 0)
const lv_font_t power_sym_14 = {
#else
lv_font_t power_sym_14 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 16,          /*The maximum line height required by the font*/
    .base_line = 3,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -5,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
    .fallback = &lv_font_montserrat_14,
};



#endif /*#if POWER_SYM_14*/

