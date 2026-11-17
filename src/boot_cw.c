/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The boot sound's text as CW elements: see boot_cw.h
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "boot_cw.h"

#include "cw_decoder/morse_code.h"

#include <stdio.h>
#include <string.h>

/* The dits an element string takes: a dit and its gap 2, a dah and its gap 4, a space 2 more */
static int code_dits(const char *code) {
    int n = 0;

    for (; *code; code++) n += *code == '-' ? 4 : 2;
    return n;
}

/* Something in text has a code: the first character the keyer knows, NULL for none */
static const cw_characters_t *keyable(const char *text) {
    for (; *text; text++) {
        const cw_characters_t *c = *text == ' ' ? NULL : morse_find(text);

        if (c) return c;
    }
    return NULL;
}

const char *boot_cw_text(int choice, const char *call, const char *own) {
    if (choice <= BOOT_SOUND_OFF) return NULL;
    if (choice == BOOT_SOUND_CALL && call && keyable(call)) return call;
    if (choice == BOOT_SOUND_TEXT && own && keyable(own)) return own;
    return BOOT_CW_CQ;
}

bool boot_cw_plan(const char *text, boot_cw_t *cw) {
    /* Where each keyed character ends: in the code, in dits, in the text */
    struct {
        size_t code, text;
        int    dits;
        char   c;       /* the character's first byte */
    } end[BOOT_CW_CODE_MAX / 2];
    size_t n = 0, len = 0;
    bool   word = false;     /* a space since the last character keyed */

    memset(cw, 0, sizeof(*cw));
    for (const char *t = text; *t && n < sizeof(end) / sizeof(end[0]);) {
        if (*t == ' ') {
            word = n > 0;
            t++;
            continue;
        }

        const cw_characters_t *c = morse_find(t);

        if (!c) {
            t++;        /* no code: left out, a letter of two bytes byte by byte */
            continue;
        }

        const char *gap = !n ? "" : word ? "   " : " ";

        if (len + strlen(gap) + strlen(c->morse) >= sizeof(cw->code)) break;
        len += snprintf(cw->code + len, sizeof(cw->code) - len, "%s%s", gap, c->morse);
        end[n].c = *t;
        t += strlen(c->character);
        end[n].code = len;
        end[n].text = (size_t)(t - text);
        end[n].dits = code_dits(cw->code);
        n++;
        word = false;
    }
    if (!n) return false;

    /* 20 WPM when it fits, else as fast as it needs up to 40 WPM, and cut after the last character that fits then,
     * not on a stroke: EA8/ON6URE/P as EA8/ON6URE */
    int dits = end[n - 1].dits;

    cw->dit_ms = BOOT_CW_DIT_MS;
    if (dits * BOOT_CW_DIT_MS > BOOT_CW_MAX_MS) {
        cw->dit_ms = BOOT_CW_MAX_MS / dits;
        if (cw->dit_ms < BOOT_CW_DIT_MIN_MS) cw->dit_ms = BOOT_CW_DIT_MIN_MS;
        size_t all = n;

        while (n > 1 && end[n - 1].dits * cw->dit_ms > BOOT_CW_MAX_MS) n--;
        while (n < all && n > 1 && end[n - 1].c == '/') n--;
    }
    cw->code[end[n - 1].code] = 0;
    cw->dits = end[n - 1].dits;
    cw->used = end[n - 1].text;
    return true;
}
