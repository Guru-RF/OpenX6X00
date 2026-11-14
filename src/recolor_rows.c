/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - coloured text in a label that wraps (see recolor_rows.h)
 *
 *  Each row is found with LVGL's own wrap (_lv_txt_get_next_line) on the colour markup the row will have, as the
 *  label wraps it when it is drawn, and ends right after its last letter, in a '\n' or the end of the text. The
 *  colour of that letter is not closed with a '#': LVGL ends it with the row (it starts every row outside any colour)
 *  and the next row opens it again. A '#' there, after a row that fills the width to the pixel and ends in a space or
 *  a comma, would be left over by LVGL's wrap, which stops there, for a row of its own. The row is then checked
 *  against LVGL's wrap as written: the last letter's kerning with the '\n' after it, not with what came next, can make
 *  it a pixel or two wider than at the cut, and a row that then did not fit would wrap again with its colour lost.
 *  Such a row is cut a pixel narrower until it fits.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "recolor_rows.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define CMD_LEN 8 /* "#rrggbb " */

/* The runs from the text's byte `from` on, each "#rrggbb text#", into out (room bytes, the 0 included). A run that
 * does not fit whole goes as far as it does, whole letters only, and nothing after it. Returns the length */
static size_t markup(char *out, size_t room, const recolor_run_t *runs, int n, size_t from) {
    size_t len = 0;

    for (int r = 0; r < n; r++) {
        const char *t     = runs[r].text;
        size_t      k     = runs[r].len, fit;
        bool        whole = true;

        if (from >= k) {
            from -= k;
            continue;
        }
        t += from;
        k -= from;
        from = 0;
        if (len + CMD_LEN + 1 + 1 + 1 > room) { /* the colour, a letter, its '#' and the 0 */
            break;
        }
        fit = room - len - CMD_LEN - 1 - 1;
        if (k > fit) {
            for (k = fit; k && ((unsigned char)t[k] & 0xC0) == 0x80; k--) { /* not half of Å */
            }
            whole = false;
        }
        if (!k) {
            break;
        }
        snprintf(out + len, CMD_LEN + 1, "#%06x ", (unsigned)(runs[r].color & 0xffffff));
        len += CMD_LEN;
        for (size_t i = 0; i < k; i++) {
            out[len++] = (unsigned char)t[i] < ' ' || t[i] == '#' ? ' ' : t[i];
        }
        out[len++] = '#';
        if (!whole) {
            break;
        }
    }
    if (room) {
        out[len] = 0;
    }
    return len;
}

/* The row: the markup (rest bytes) up to where LVGL cuts it (m), one letter at least, and no further than its last
 * letter: a colour begun after it is left out, and so is the '#' that ends that letter's colour. *shown: the text's
 * bytes on it. Returns its length */
static size_t cut(const char *row, size_t rest, uint32_t m, size_t *shown) {
    lv_text_cmd_state_t st     = LV_TEXT_CMD_STATE_WAIT;
    size_t              open   = 0;        /* where the colour open now begins */
    size_t              in_run = 0;        /* the bytes shown in it */
    size_t              close  = SIZE_MAX; /* where the last colour ended */
    size_t              i;

    *shown = 0;
    for (i = 0; i < rest; i++) {
        unsigned char       c   = row[i];
        lv_text_cmd_state_t was = st;

        if (i >= m && *shown && (c & 0xC0) != 0x80) {
            break;
        }
        if (_lv_txt_is_cmd(&st, c)) {
            if (was == LV_TEXT_CMD_STATE_WAIT) {
                open   = i;
                in_run = 0;
            } else if (was == LV_TEXT_CMD_STATE_IN) {
                close = i;
            }
            continue;
        }
        (*shown)++;
        in_run++;
    }
    if (st != LV_TEXT_CMD_STATE_WAIT && !(st == LV_TEXT_CMD_STATE_IN && in_run)) {
        i = open;
    }
    if (close != SIZE_MAX && close + 1 == i) {
        i = close;
    }
    return i;
}

size_t recolor_rows(char *buf, size_t size, const recolor_run_t *runs, int n, const lv_font_t *font, lv_coord_t ls,
                    lv_coord_t width) {
    size_t total = 0, from = 0, len = 0;

    if (!size) {
        return 0;
    }
    buf[0] = 0;
    for (int r = 0; r < n; r++) {
        total += runs[r].len;
    }
    while (from < total && len + CMD_LEN + 4 <= size) {
        char      *row  = buf + len;
        lv_coord_t w    = width;
        size_t     rlen = 0, shown = 0;

        for (;;) {
            /* the rest from here, room kept for the '\n' after the cut */
            size_t   rest = markup(row, size - len - 1, runs, n, from);
            uint32_t m;

            if (!rest) {
                break;
            }
            m    = _lv_txt_get_next_line(row, font, ls, w, NULL, LV_TEXT_FLAG_RECOLOR);
            rlen = cut(row, rest, m, &shown);
            if (from + shown < total) {
                row[rlen++] = '\n';
            }
            row[rlen] = 0;
            if (w <= 1 || _lv_txt_get_next_line(row, font, ls, width, NULL, LV_TEXT_FLAG_RECOLOR) == rlen) {
                break;
            }
            w--;
        }
        if (!shown) {
            break;
        }
        len += rlen;
        from += shown;
    }
    if (len && buf[len - 1] == '\n') { /* full before the end: a '\n' last would make an empty row */
        len--;
    }
    buf[len] = 0;
    return len;
}
