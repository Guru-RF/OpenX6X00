/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the Zello app's Simplex repeater rules. See zello_link.h.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "zello_link.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

void zello_link_init(zello_link_t *l) {
    memset(l, 0, sizeof(*l));
    l->state = ZELLO_LINK_IDLE;
}

void zello_link_rearm(zello_link_t *l) {
    l->to_rf_stopped = false;
}

static bool id_due(const zello_link_t *l, const zello_link_cfg_t *c, uint64_t now) {
    return c->id_on && (l->id_ms == 0 || now - l->id_ms >= c->id_period_ms);
}

static void go(zello_link_t *l, zello_link_state_t s, uint64_t now) {
    l->state    = s;
    l->since_ms = now;
}

/* Our transmission has ended (its thread is over) */
static void tx_over(zello_link_t *l, const zello_link_in_t *in, zello_link_out_t *out) {
    switch (in->result) {
        case ZELLO_TX_NOT_KEYED:        /* nothing went: as before it */
            break;
        case ZELLO_TX_STOPPED:
            l->air_end_ms    = in->now_ms;
            l->to_rf_stopped = true;
            l->tx_since_id   = false;   /* no ID to follow: the radio would stop it too */
            out->why         = "Zello to RF stopped: the radio ended the transmission";
            break;
        case ZELLO_TX_DONE:
            l->air_end_ms = in->now_ms;
            if (l->id_pending) {
                l->id_ms       = in->now_ms;
                l->tx_since_id = false;
            } else {
                l->tx_since_id = true;
            }
            break;
    }
    l->id_pending = false;
    go(l, ZELLO_LINK_IDLE, in->now_ms);
}

zello_link_out_t zello_link_step(zello_link_t *l, const zello_link_cfg_t *c, const zello_link_in_t *in) {
    zello_link_out_t out = {0};
    uint64_t         now = in->now_ms;

    switch (l->state) {
        case ZELLO_LINK_RELAY:
        case ZELLO_LINK_ID:
            if (!in->keyed) {
                tx_over(l, in, &out);
                break;
            }
            if (in->operator_key) {     /* the operator's key wins: off at once, the rest of the message not sent */
                out.unkey     = true;
                out.now       = true;
                l->cut        = l->cut || l->state == ZELLO_LINK_RELAY;
                l->id_pending = false;
                go(l, ZELLO_LINK_ENDING, now);
                break;
            }
            if (l->state == ZELLO_LINK_RELAY) {
                bool limit = now - l->since_ms >= c->limit_ms;

                if (limit || !in->zello_msg || !c->to_rf || !in->connected) {
                    out.unkey     = true;
                    out.now       = in->zello_msg;  /* over: what is queued plays out; else it is dropped */
                    out.id        = id_due(l, c, now);
                    l->id_pending = out.id;
                    if (limit) {
                        l->cut  = true;
                        out.why = "Zello to RF: time limit, unkeyed";
                    } else if (!in->zello_msg) {
                        /* over */
                    } else {
                        l->cut = true;  /* switched off or off the channel: the rest of it not sent */
                    }
                    go(l, ZELLO_LINK_ENDING, now);
                }
            }
            break;

        case ZELLO_LINK_ENDING:
            if (!in->keyed) {
                tx_over(l, in, &out);
            } else if (in->operator_key && l->id_pending) {
                out.unkey     = true;   /* the ID too: the operator is on the air with his own call */
                out.now       = true;
                l->id_pending = false;
            }
            break;

        case ZELLO_LINK_TO_ZELLO: {
            bool stop = false;

            if (in->radio_busy || in->keyed) {
                stop = true;            /* never while the radio transmits: what it receives then is not the frequency */
            } else if (in->rx_ended) {
                stop      = true;       /* the helper ended it (refused, two minutes): not again until the squelch closes */
                l->rx_cut = true;
            } else if (!c->to_zello || !in->connected || !in->squelch_open) {
                stop = true;
            } else if (now - l->since_ms >= c->limit_ms) {
                stop      = true;
                l->rx_cut = true;
                out.why   = "RF to Zello: time limit, until the squelch closes";
            }
            if (stop) {
                out.talk_stop = !in->rx_ended;
                go(l, ZELLO_LINK_IDLE, now);
            }
            break;
        }

        case ZELLO_LINK_IDLE:
            break;
    }

    if (l->state != ZELLO_LINK_IDLE || out.unkey || out.talk_stop || out.key) {
        return out;
    }

    if (!in->zello_msg) {
        l->cut = false;
    }
    if (!in->squelch_open) {
        l->rx_cut = false;
    }

    bool free = in->connected && !in->radio_busy && !in->keyed &&
                (l->air_end_ms == 0 || now - l->air_end_ms >= ZELLO_LINK_HOLD_MS);

    if (c->to_zello && free && in->squelch_open && !l->rx_cut) {
        out.talk_start = true;
        go(l, ZELLO_LINK_TO_ZELLO, now);
    } else if (c->to_rf && !l->to_rf_stopped && in->can_key && free && !in->squelch_open && in->zello_msg && !l->cut &&
               (l->air_end_ms == 0 || now - l->air_end_ms >= c->pause_ms)) {
        out.key = true;
        go(l, ZELLO_LINK_RELAY, now);
    } else if (l->tx_since_id && id_due(l, c, now) && in->can_key && free && !in->squelch_open) {
        out.key       = true;
        out.id        = true;
        l->id_pending = true;
        go(l, ZELLO_LINK_ID, now);
    }
    return out;
}

bool zello_sql_step(zello_sql_t *s, int rms, uint32_t ms, uint32_t hang_ms) {
    if (!s->open) {
        s->loud_ms = rms >= ZELLO_SQL_OPEN ? s->loud_ms + ms : 0;
        if (s->loud_ms >= ZELLO_SQL_OPEN_MS) {
            s->open     = true;
            s->quiet_ms = 0;
        }
    } else {
        s->quiet_ms = rms < ZELLO_SQL_CLOSE ? s->quiet_ms + ms : 0;
        if (s->quiet_ms >= hang_ms) {
            s->open    = false;
            s->loud_ms = 0;
        }
    }
    return s->open;
}

/* ---------- Morse ---------- */

static const char *morse_of(char c) {
    static const char *const letters[26] = {".-",   "-...", "-.-.", "-..",  ".",   "..-.", "--.",  "....", "..",
                                            ".---", "-.-",  ".-..", "--",   "-.",  "---",  ".--.", "--.-", ".-.",
                                            "...",  "-",    "..-",  "...-", ".--", "-..-", "-.--", "--.."};
    static const char *const digits[10]  = {"-----", ".----", "..---", "...--", "....-",
                                            ".....", "-....", "--...", "---..", "----."};

    c = (char)toupper((unsigned char)c);
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    if (c >= '0' && c <= '9') return digits[c - '0'];
    switch (c) {
        case '/': return "-..-.";
        case '-': return "-....-";
        case '?': return "..--..";
        case '.': return ".-.-.-";
        case ',': return "--..--";
        default:  return NULL;
    }
}

/* Calls each(on, units, ctx) for the key down (on) and up, in dot units: a dot 1, a dash 3, between elements 1,
 * between letters 3, between words 7 */
static void morse_walk(const char *text, void (*each)(bool on, int units, void *ctx), void *ctx) {
    bool letter_before = false;
    bool space         = false;

    for (const char *p = text; *p; p++) {
        const char *m = morse_of(*p);

        if (*p == ' ') {
            space = letter_before;
            continue;
        }
        if (!m) continue;
        if (letter_before) each(false, space ? 7 : 3, ctx);
        for (const char *e = m; *e; e++) {
            if (e != m) each(false, 1, ctx);
            each(true, *e == '-' ? 3 : 1, ctx);
        }
        letter_before = true;
        space         = false;
    }
}

static void count_units(bool on, int units, void *ctx) {
    (void)on;
    *(int *)ctx += units;
}

uint32_t zello_morse_ms(const char *text, int wpm) {
    int units = 0;

    morse_walk(text, count_units, &units);
    return (uint32_t)units * 1200u / (uint32_t)wpm;
}

typedef struct {
    int16_t *out;
    size_t   max, n;
    int      unit;          /* samples a dot */
    int      edge;          /* samples of a soft edge */
    double   phase, step;
    float    amp;
} render_t;

static void render_units(bool on, int units, void *ctx) {
    render_t *r     = ctx;
    int       total = units * r->unit;

    for (int i = 0; i < total && r->n < r->max; i++) {
        float v = 0.0f;

        if (on) {
            float g = 1.0f;

            if (i < r->edge) g = 0.5f - 0.5f * cosf((float)M_PI * i / r->edge);
            else if (total - 1 - i < r->edge) g = 0.5f - 0.5f * cosf((float)M_PI * (total - 1 - i) / r->edge);
            v = r->amp * g * (float)sin(r->phase);
            r->phase += r->step;
            if (r->phase > 2.0 * M_PI) r->phase -= 2.0 * M_PI;
        }
        r->out[r->n++] = (int16_t)(v * 32767.0f);
    }
}

size_t zello_morse_render(const char *text, int wpm, int tone_hz, int rate, float amp, int16_t *out, size_t max) {
    render_t r = {
        .out  = out,
        .max  = max,
        .unit = rate * 1200 / wpm / 1000,
        .edge = rate * 5 / 1000,
        .step = 2.0 * M_PI * tone_hz / rate,
        .amp  = amp,
    };

    morse_walk(text, render_units, &r);
    return r.n;
}
