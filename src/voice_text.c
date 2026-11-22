/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - decoded text read aloud (voice_text.h)
 *
 *  The decoders hand their text over a character or a few at a time; it is gathered here on the GUI thread and said
 *  in bursts, with the callsigns and grids in it spelled out (voice_spell_calls). A burst waits for the announcement
 *  before it to end: decoded text never cuts itself off, though a key or a knob still cuts it.
 */

#include "voice_text.h"

#include <stdio.h>
#include <string.h>

#include "cfg/cfg_api.h"
#include "lvgl/lvgl.h"
#include "scheduler.h"
#include "voice.h"

#define IDLE_MS   1200      /* nothing new for this long: the burst is said */
#define BURST_MAX 90        /* a burst this long is said at its next space */

static char        pend[400];
static size_t      pend_n;
static uint32_t    last_ms;
static bool        line_end;
static lv_timer_t *tick_timer;

static bool reading_on(void) {
    return param_i_get(cfg_voice_read_text) && voice_auto_active();
}

/* Noise a decoder makes of static: one letter over and over ("TTTT", "EEE"), and bursts of nothing but the
 * shortest Morse letters (E T I M N A), alone or in pairs ("T E TI"). Real text keeps its short words. */
static bool word_repeats(const char *w, size_t n) {
    if (n < 2 || !((w[0] >= 'A' && w[0] <= 'Z') || (w[0] >= 'a' && w[0] <= 'z'))) {
        return false;
    }
    for (size_t i = 1; i < n; i++) {
        if ((w[i] | 0x20) != (w[0] | 0x20)) {
            return false;
        }
    }
    return true;
}

static bool word_short_noise(const char *w, size_t n) {
    if (n > 2) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!strchr("ETIMNAetimna", w[i])) {
            return false;
        }
    }
    return true;
}

static void drop_noise(void) {
    char   out[sizeof(pend)];
    size_t o    = 0;
    bool   real = false;

    for (size_t i = 0; i < pend_n;) {
        size_t j = i;

        while (j < pend_n && pend[j] != ' ') {
            j++;
        }
        if (j > i && !word_repeats(pend + i, j - i)) {
            if (!word_short_noise(pend + i, j - i)) {
                real = true;
            }
            if (o) {
                out[o++] = ' ';
            }
            memcpy(out + o, pend + i, j - i);
            o += j - i;
        }
        i = j + 1;
    }
    if (!real) {
        o = 0;
    }
    memcpy(pend, out, o);
    pend_n       = o;
    pend[pend_n] = '\0';
}

static void flush(void) {
    char spelled[sizeof(pend) * 2];

    pend[pend_n] = '\0';
    while (pend_n && pend[pend_n - 1] == ' ') {
        pend[--pend_n] = '\0';
    }
    drop_noise();
    if (pend_n) {
        /* spelled word by word, then the words joined with spaces again: a text is read as a sentence */
        voice_spell_calls(pend, spelled, sizeof(spelled));
        for (char *p = spelled; (p = strstr(p, ", ")) != NULL;) {
            memmove(p, p + 1, strlen(p + 1) + 1);
        }
        voice_say_text_fmt("%s", spelled);
    }
    pend_n   = 0;
    pend[0]  = '\0';
    line_end = false;
}

static void tick_cb(lv_timer_t *t) {
    (void)t;
    if (!pend_n && voice_is_on()) {
        return;
    }
    if (!reading_on()) {
        pend_n   = 0;
        line_end = false;
        if (!voice_is_on()) {       /* speech switched off: the timer goes, until text is read again */
            lv_timer_del(tick_timer);
            tick_timer = NULL;
        }
        return;
    }
    if (voice_busy()) {
        return;     /* the announcement before it first; the text keeps gathering */
    }
    if (line_end || lv_tick_elaps(last_ms) >= IDLE_MS ||
        (pend_n >= BURST_MAX && pend[pend_n - 1] == ' ') || pend_n + 2 >= sizeof(pend)) {
        flush();
    }
}

static void add_job(void *arg) {
    const char *s = arg;

    if (!reading_on()) {
        return;
    }
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;

        if (c == '\n' || c == '\r') {
            if (pend_n) {
                line_end = true;
                c = ' ';
            } else {
                continue;
            }
        }
        if (c < ' ' || c > '~') {
            continue;
        }
        if (c == ' ' && (pend_n == 0 || pend[pend_n - 1] == ' ')) {
            continue;
        }
        if (pend_n + 2 >= sizeof(pend)) {
            break;      /* tick_cb() says it as soon as it can */
        }
        pend[pend_n++] = (char)c;
        pend[pend_n]   = '\0';
    }
    last_ms = lv_tick_get();
    if (!tick_timer) {
        tick_timer = lv_timer_create(tick_cb, 250, NULL);
    }
}

void voice_text_add(const char *text) {
    if (text && text[0] && voice_is_on()) {     /* off: not even queued (a decoder's thread, every character) */
        scheduler_put(add_job, (void *)text, strlen(text) + 1);
    }
}

typedef struct {
    uint32_t id;
    char     text[200];
} msg_job_t;

static void msg_job(void *arg) {
    static uint32_t said_id;
    static bool     said_any;
    msg_job_t      *m = arg;

    if (said_any && m->id == said_id) {
        return;
    }
    said_id  = m->id;
    said_any = true;
    add_job(m->text);
    line_end = true;    /* a message is one burst */
}

void voice_text_message(uint32_t id, const char *text, bool complete) {
    msg_job_t m = {.id = id};

    if (!complete || !text || !text[0] || !voice_is_on()) {
        return;
    }
    snprintf(m.text, sizeof(m.text), "%s", text);
    scheduler_put(msg_job, &m, sizeof(m));
}
