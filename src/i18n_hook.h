/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - every GUI source file gets this first (CMake: -include i18n_hook.h): the texts it hands to LVGL go
 *  through i18n_tr(), so a lv_label_set_text() needs no change to be translated. LVGL's own sources do not get it.
 *  A text the language file lacks shows as before. Not lv_txt_get_width(): its callers pass a byte length, which a
 *  translation changes. After the CX8ANM edition's (condoruy), see i18n.h.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#pragma once

/* This comes before the file's own lines, its system headers too (LVGL's): a file that asks for GNU's functions
 * (#define _GNU_SOURCE, as twenty of ours do: accept4, strcasestr, exp10f) would ask too late. So for all */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lvgl/lvgl.h"
#include "i18n.h"

#define lv_label_set_text(obj, txt)                      lv_label_set_text(obj, i18n_tr(txt))
#define lv_label_set_text_static(obj, txt)               lv_label_set_text_static(obj, i18n_tr_static(txt))
#define lv_label_set_text_fmt(obj, fmt, ...)             lv_label_set_text_fmt(obj, i18n_tr(fmt), ##__VA_ARGS__)
#define lv_dropdown_set_options(obj, opts)               lv_dropdown_set_options(obj, i18n_tr(opts))
#define lv_dropdown_set_options_static(obj, opts)        lv_dropdown_set_options_static(obj, i18n_tr_static(opts))
#define lv_dropdown_add_option(obj, opt, pos)            lv_dropdown_add_option(obj, i18n_tr(opt), pos)
#define lv_dropdown_set_text(obj, txt)                   lv_dropdown_set_text(obj, i18n_tr_static(txt))
#define lv_textarea_set_placeholder_text(obj, txt)       lv_textarea_set_placeholder_text(obj, i18n_tr(txt))
#define lv_list_add_text(list, txt)                      lv_list_add_text(list, i18n_tr(txt))
#define lv_list_add_btn(list, icon, txt)                 lv_list_add_btn(list, icon, i18n_tr(txt))
#define lv_checkbox_set_text(obj, txt)                   lv_checkbox_set_text(obj, i18n_tr(txt))
#define lv_draw_label(ctx, dsc, area, txt, hint)         lv_draw_label(ctx, dsc, area, i18n_tr(txt), hint)
#define lv_canvas_draw_text(c, x, y, w, dsc, txt)        lv_canvas_draw_text(c, x, y, w, dsc, i18n_tr(txt))
#define lv_txt_get_size(size, txt, font, ls, lsp, mw, f) lv_txt_get_size(size, i18n_tr(txt), font, ls, lsp, mw, f)
#define lv_table_set_cell_value(obj, r, c, txt)          lv_table_set_cell_value(obj, r, c, i18n_tr(txt))
#define lv_table_set_cell_value_fmt(obj, r, c, fmt, ...) lv_table_set_cell_value_fmt(obj, r, c, i18n_tr(fmt), ##__VA_ARGS__)
