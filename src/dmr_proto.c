/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - what the DMR app reads of its helper (dmr_proto.h). A line it does not know changes nothing: an older
 *  GUI with a newer helper goes on.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "dmr_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void dmr_proto_init(dmr_state_t *s) {
    memset(s, 0, sizeof(*s));
}

/* The next word of *p, NUL-ended in place; "" at the end */
static char *word(char **p) {
    char *w = *p;

    while (*w == ' ') w++;
    char *e = w;

    while (*e && *e != ' ') e++;
    if (*e) *e++ = 0;
    *p = e;
    return w;
}

static bool number(const char *w, uint32_t *out) {
    char         *end;
    unsigned long v;

    if (!*w || *w < '0' || *w > '9') return false;
    v = strtoul(w, &end, 10);
    if (*end || v > UINT32_MAX) return false;
    *out = (uint32_t)v;
    return true;
}

/* What is left of the line, its spaces at the start skipped, as text of at most n - 1 bytes */
static void rest(char *p, char *out, size_t n) {
    while (*p == ' ') p++;
    snprintf(out, n, "%s", p);
}

static void heard_put(dmr_state_t *s, const dmr_call_t *c) {
    int k = 0;

    while (k < s->n_heard && s->heard[k].id != c->id) k++;
    if (k == s->n_heard && k == DMR_HEARD_MAX) k--;     /* full: the oldest goes */
    else if (k == s->n_heard) s->n_heard++;
    memmove(&s->heard[1], &s->heard[0], (size_t)k * sizeof(s->heard[0]));
    s->heard[0] = *c;
}

dmr_ev_t dmr_proto_line(dmr_state_t *s, const char *line, uint64_t now_ms) {
    char     buf[256], *p = buf, *w;
    uint32_t a, b, slot;

    snprintf(buf, sizeof(buf), "%s", line);
    buf[strcspn(buf, "\r\n")] = 0;
    w = word(&p);

    if (!strcmp(w, "state")) {
        w = word(&p);
        if (!strcmp(w, "connecting")) {
            s->conn    = DMR_CONN_CONNECTING;
            s->why[0]  = 0;
            s->talking = false;
            return DMR_EV_NONE;
        }
        if (!strcmp(w, "online")) {
            snprintf(s->master, sizeof(s->master), "%s", word(&p));
            if (!number(word(&p), &s->hotspot)) s->hotspot = 0;
            if (!number(word(&p), &s->tg)) s->tg = 0;
            s->conn   = DMR_CONN_ONLINE;
            s->why[0] = 0;
            return DMR_EV_ONLINE;
        }
        if (!strcmp(w, "retry")) {
            bool was = s->talking;

            s->retry_s = number(word(&p), &a) ? (int)a : 0;
            rest(p, s->why, sizeof(s->why));
            s->conn    = DMR_CONN_RETRY;
            s->talking = false;
            return was ? DMR_EV_LOST : DMR_EV_NONE;
        }
        return DMR_EV_NONE;
    }
    if (!strcmp(w, "error")) {
        rest(p, s->why, sizeof(s->why));
        s->conn    = DMR_CONN_ERROR;
        s->talking = false;
        return DMR_EV_ERROR;
    }
    if (!strcmp(w, "call")) {
        dmr_call_t c = {0};
        char      *kind;

        if (!number(word(&p), &a) || !number(word(&p), &b)) return DMR_EV_NONE;
        kind = word(&p);
        if (!number(word(&p), &slot) || slot < 1 || slot > 2) return DMR_EV_NONE;
        c.id           = a;
        c.dst          = b;
        c.private_call = !strcmp(kind, "private");
        c.slot         = (uint8_t)slot;
        c.start_ms     = now_ms;
        rest(p, c.name, sizeof(c.name));
        s->talker  = c;
        s->talking = true;
        return DMR_EV_CALL;
    }
    if (!strcmp(w, "name")) {
        if (!number(word(&p), &a)) return DMR_EV_NONE;
        char name[DMR_NAME_MAX];

        rest(p, name, sizeof(name));
        if (s->talking && s->talker.id == a) snprintf(s->talker.name, sizeof(s->talker.name), "%s", name);
        for (int k = 0; k < s->n_heard; k++)
            if (s->heard[k].id == a) snprintf(s->heard[k].name, sizeof(s->heard[k].name), "%s", name);
        return DMR_EV_NONE;
    }
    if (!strcmp(w, "end")) {
        if (!number(word(&p), &a) || !s->talking || s->talker.id != a) return DMR_EV_NONE;
        double secs = atof(word(&p));

        s->talker.secs = secs > 0 ? (uint32_t)(secs + 0.5) : 0;
        s->talking     = false;
        heard_put(s, &s->talker);
        return DMR_EV_END;
    }
    return DMR_EV_NONE;
}

const char *dmr_proto_who(const dmr_call_t *c, char *buf, size_t n) {
    if (c->name[0]) snprintf(buf, n, "%s", c->name);
    else snprintf(buf, n, "%u", (unsigned)c->id);
    return buf;
}
