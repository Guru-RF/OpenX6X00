/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the GUI's languages (#256)
 *
 *  English is the key: i18n_tr("Settings") gives the text in the language chosen, or the English as it is when the
 *  language file has no translation for it (numbers, callsigns and frequencies go through untouched). A text of
 *  several lines is looked up whole, then line by line ("Filter low:\n200 Hz" takes "Filter low"), with LVGL's colour
 *  spans kept ("#ffffff Volume:#"). i18n_hook.h passes every text the GUI hands to LVGL through here, so most code
 *  needs nothing; a text made with snprintf before it reaches LVGL marks its format with _().
 *
 *  One gettext file per language, read when it is chosen: BOARD_SHARE/i18n/<code>.po as the image ships it, and a
 *  <code>.po in BOARD_DATA/i18n (the DATA partition) before it, so a translator checks a file on the radio without a
 *  build. A translation whose printf conversions differ from the English's is left out (it would crash a format);
 *  a "fuzzy" one too, as gettext does. A language whose script our fonts lack names its font in the header
 *  (X-OpenX6X00-Font: a TrueType file beside the .po or in BOARD_SHARE/fonts): font_select.c puts it behind every
 *  font.
 *
 *  The way texts reach LVGL through a hook, and their matching line by line and around the colour spans, come from
 *  the CX8ANM edition of OpenX6X00 (condoruy, Cóndor Tecnológico), whose Spanish is the first translation here.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The text in the language chosen, else s. The result of a text put together line by line lives for a while (LVGL
 * copies a label's text at once); one to keep: i18n_tr_static(). Any thread */
const char *i18n_tr(const char *s);
const char *i18n_tr_static(const char *s);

/* gettext's marks, which tools/i18n/extract.py looks for: _() translates, N_() only marks a text of a table that is
 * translated where it is shown */
#define _(s)  i18n_tr(s)
#define N_(s) (s)

typedef struct {
    char code[16];      /* the file's name: es, fr, nl, zh_CN; "" for English */
    char name[48];      /* in its own language, for the list: Español, 中文 (X-OpenX6X00-Name) */
    char status[16];    /* X-OpenX6X00-Status: "" or "draft" */
    bool user;          /* from BOARD_DATA/i18n, a translator's copy */
} i18n_lang_t;

/* The language of the file <code>.po; "" or "en": English. False: no such file (English then). LVGL thread, before
 * the screen is built again */
bool i18n_load(const char *code);
const char *i18n_code(void);

/* The TrueType file the language asks for behind our fonts, a path; NULL: none */
const char *i18n_font(void);

/* English first, then each language file found (BOARD_DATA/i18n before BOARD_SHARE/i18n), at most max */
int i18n_list(i18n_lang_t *out, int max);

/* The capitals of a UTF-8 text, for the themes and places that show capitals: Latin-1's letters too (é as É; ß
 * stays), œ, ÿ; another script as it is. ascii false: the ASCII letters left as they are (the caller did them).
 * The text keeps its length, so out may be in; cut at size on a whole character */
void i18n_upper(char *out, size_t size, const char *in, bool ascii);

#ifdef __cplusplus
}
#endif
