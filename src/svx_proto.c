/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the SVXConnect helper's lines read into the window's state. See svx_proto.h.
 *
 *  Each list line replaces its list whole: an entry that does not read is left out, the others kept. A callsign is
 *  whatever lies between the first field and the numbers after it, so a ':' in one does not shift the numbers.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "svx_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void svx_proto_init(svx_state_t *s) {
    memset(s, 0, sizeof(*s));
    s->nodes = -1;
}

/* Copy at most cap - 1 bytes of the n at p, printable ASCII and UTF-8 only */
static void put(char *dst, size_t cap, const char *p, size_t n) {
    size_t k = 0;

    for (size_t i = 0; i < n && k + 1 < cap; i++) {
        unsigned char c = (unsigned char)p[i];

        dst[k++] = c < 0x20 || c == 0x7f ? '?' : (char)c;
    }
    dst[k] = 0;
}

static bool num(const char *p, const char *end, uint64_t *out) {
    uint64_t v = 0;

    if (p >= end) return false;
    for (; p < end; p++) {
        if (*p < '0' || *p > '9' || v > UINT64_MAX / 10 - 9) return false;
        v = v * 10 + (uint64_t)(*p - '0');
    }
    *out = v;
    return true;
}

/* The next space-separated item of *p: [*b, *e), false at the end */
static bool item(const char **p, const char **b, const char **e) {
    const char *s = *p;

    while (*s == ' ') s++;
    if (!*s) return false;
    *b = s;
    while (*s && *s != ' ') s++;
    *e = *p = s;
    return true;
}

/* "<tg>:<call>:<n>[:<m>]": the tg, the call, and the last numbers (want of them) */
static bool split(const char *b, const char *e, int want, uint32_t *tg, char *call, uint64_t *n1, uint64_t *n2) {
    const char *c1 = memchr(b, ':', (size_t)(e - b));
    const char *cut[2];
    const char *q = e;
    uint64_t    v;

    if (!c1 || !num(b, c1, &v) || v > UINT32_MAX) return false;
    *tg = (uint32_t)v;
    for (int i = 0; i < want; i++) {          /* from the right */
        while (q > c1 + 1 && q[-1] != ':') q--;
        if (q <= c1 + 1) return false;
        cut[i] = --q;                           /* the ':' before the number */
    }
    if (want == 1) {
        if (!num(cut[0] + 1, e, n1)) return false;
    } else {
        if (!num(cut[1] + 1, cut[0], n1) || !num(cut[0] + 1, e, n2)) return false;
    }
    const char *ce = cut[want - 1];

    if (ce <= c1 + 1) return false;             /* no call */
    put(call, SVX_CALL_LEN, c1 + 1, (size_t)(ce - c1 - 1));
    return true;
}

static void list_tgs(svx_state_t *s, const char *p) {
    const char *b, *e;

    s->n_tgs = 0;
    while (s->n_tgs < SVX_TGS_MAX && item(&p, &b, &e)) {
        const char *c1 = memchr(b, ':', (size_t)(e - b)), *c2;
        uint64_t    id, prio;
        svx_tg_t   *t = &s->tgs[s->n_tgs];

        if (!c1 || !(c2 = memchr(c1 + 1, ':', (size_t)(e - c1 - 1)))) continue;
        if (!num(b, c1, &id) || id == 0 || id > UINT32_MAX || !num(c1 + 1, c2, &prio) || prio > 9) continue;
        t->id    = (uint32_t)id;
        t->prio  = (int)prio;
        t->sw    = memchr(c2 + 1, 's', (size_t)(e - c2 - 1)) != NULL;
        t->muted = memchr(c2 + 1, 'm', (size_t)(e - c2 - 1)) != NULL;
        s->n_tgs++;
    }
}

static void list_active(svx_state_t *s, const char *p, uint64_t now) {
    const char *b, *e;

    s->n_active = 0;
    while (s->n_active < SVX_ACTIVE_MAX && item(&p, &b, &e)) {
        svx_talker_t *t = &s->active[s->n_active];
        uint64_t      ms, unused;

        if (!split(b, e, 1, &t->tg, t->call, &ms, &unused)) continue;
        t->since_ms = now >= ms ? now - ms : 0;
        s->n_active++;
    }
}

static void list_recent(svx_state_t *s, const char *p, uint64_t now) {
    const char *b, *e;

    s->n_recent = 0;
    while (s->n_recent < SVX_RECENT_MAX && item(&p, &b, &e)) {
        svx_recent_t *r = &s->recent[s->n_recent];
        uint64_t      ago, dur;

        if (!split(b, e, 2, &r->tg, r->call, &ago, &dur) || dur > UINT32_MAX) continue;
        r->end_ms = now >= ago * 1000 ? now - ago * 1000 : 0;
        r->dur_s  = (uint32_t)dur;
        s->n_recent++;
    }
}

/* "<word> " at the start of line: what follows, else NULL; the word alone: "" */
static const char *after(const char *line, const char *word) {
    size_t n = strlen(word);

    if (strncmp(line, word, n)) return NULL;
    if (line[n] == 0) return line + n;
    return line[n] == ' ' ? line + n + 1 : NULL;
}

svx_ev_t svx_proto_line(svx_state_t *s, const char *line, uint64_t now_ms, char *why, size_t why_n) {
    const char *a;

    if (why && why_n) why[0] = 0;
    if ((a = after(line, "hello"))) {
        put(s->version, sizeof(s->version), a, strlen(a));
    } else if ((a = after(line, "error"))) {
        put(s->error, sizeof(s->error), a, strlen(a));
        if (why) put(why, why_n, a, strlen(a));
        return SVX_EV_ERROR;
    } else if ((a = after(line, "conn"))) {
        const char *sp = strchr(a, ' ');
        size_t      w  = sp ? (size_t)(sp - a) : strlen(a);

        if (w == 9 && !strncmp(a, "connected", 9)) s->conn = SVX_CONN_CONNECTED;
        else if (w == 10 && !strncmp(a, "connecting", 10)) s->conn = SVX_CONN_CONNECTING;
        else if (w == 12 && !strncmp(a, "reconnecting", 12)) s->conn = SVX_CONN_RECONNECTING;
        else if (w == 4 && !strncmp(a, "idle", 4)) s->conn = SVX_CONN_IDLE;
        else return SVX_EV_NONE;
        put(s->why, sizeof(s->why), sp ? sp + 1 : "", sp ? strlen(sp + 1) : 0);
        if (s->conn != SVX_CONN_CONNECTED) s->n_active = 0;
    } else if ((a = after(line, "server"))) {
        char     host[64];
        unsigned port, id;
        int      nodes;

        if (sscanf(a, "%63s %u %u %d", host, &port, &id, &nodes) == 4) {
            put(s->host, sizeof(s->host), host, strlen(host));
            s->port      = port;
            s->client_id = id;
            s->nodes     = nodes;
        }
    } else if ((a = after(line, "tg"))) {
        unsigned tg;
        int      locked;

        if (sscanf(a, "%u %d", &tg, &locked) == 2) {
            s->tg     = tg;
            s->locked = locked != 0;
        }
    } else if ((a = after(line, "tgs"))) {
        list_tgs(s, a);
    } else if ((a = after(line, "active"))) {
        list_active(s, a, now_ms);
    } else if ((a = after(line, "recent"))) {
        list_recent(s, a, now_ms);
    } else if ((a = after(line, "tx"))) {
        const char *w;

        if ((w = after(a, "on"))) {
            s->tx    = true;
            s->tx_tg = (uint32_t)strtoul(w, NULL, 10);
            return SVX_EV_TX_ON;
        }
        if ((w = after(a, "off"))) {
            bool was = s->tx;

            s->tx = false;
            if (why) put(why, why_n, w, strlen(w));
            return was ? SVX_EV_TX_OFF : SVX_EV_NONE;
        }
    } else if ((a = after(line, "refused"))) {
        if (why) put(why, why_n, a, strlen(a));
        return SVX_EV_REFUSED;
    } else if ((a = after(line, "cert"))) {
        char      st[24] = "";
        long long end    = 0;
        int       days   = 0;
        /* the status may be two words ("renewal due", "not yet valid"): the two numbers from the right */
        const char *n2 = strrchr(a, ' ');

        if (n2 && n2 > a) {
            const char *n1 = n2 - 1;

            while (n1 > a && *n1 != ' ') n1--;
            if (n1 > a && sscanf(n1 + 1, "%lld %d", &end, &days) == 2) {
                put(st, sizeof(st), a, (size_t)(n1 - a));
                snprintf(s->cert, sizeof(s->cert), "%s", st);
                s->cert_end  = end;
                s->cert_days = days;
            }
        }
    } else if ((a = after(line, "banner"))) {
        put(s->banner, sizeof(s->banner), a, strlen(a));
    }
    return SVX_EV_NONE;
}

const svx_talker_t *svx_proto_talker_on(const svx_state_t *s, uint32_t tg) {
    for (int i = 0; i < s->n_active; i++) {
        if (s->active[i].tg == tg) return &s->active[i];
    }
    return NULL;
}

const svx_talker_t *svx_proto_talker(const svx_state_t *s) {
    return s->tg ? svx_proto_talker_on(s, s->tg) : NULL;
}

int svx_proto_tg_index(const svx_state_t *s, uint32_t tg) {
    for (int i = 0; i < s->n_tgs; i++) {
        if (s->tgs[i].id == tg) return i;
    }
    return -1;
}
