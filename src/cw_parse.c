/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW text parser (cw_parse.h). A run-together word splits only when all of it reads as known
 *  tokens and callsigns; the cheapest reading wins: a token costs 10, a number 20, a call 22,
 *  a call with a four-letter suffix 36 (so "ON6UREK" reads ON6URE K) and one of three
 *  characters 27, one already in the text or known (cw_parse_know) 8 ("DEW1AWK" reads DE W1AW K
 *  once W1AW was sent, else W1AWK stays one call). After NAME, NM, QTH or WX the rest of the word
 *  may be a name ("NAMEGIO").
 */

#include "cw_parse.h"

#include <stdint.h>
#include <string.h>

#define MAX_WORD    64      /* longer words stay as they are */
#define MAX_SEG     14      /* EA8/IK1ABC/QRP */
#define FREE_REST   30      /* the name after a label */

static const char *const tokens[] = {
    "CQ", "DE", "K", "KN", "BK", "AR", "SK", "R", "RR", "TU", "TNX", "TKS", "73", "88", "UR", "RST",
    "QTH", "NAME", "NM", "OP", "RIG", "ANT", "PWR", "WX", "PSE", "AGN", "QSL", "QSO", "QRZ", "QRZ?",
    "QRL", "QRL?", "QRM", "QRN", "QSB", "QRP", "QRS", "QRQ", "QSY", "QRT", "GM", "GA", "GE", "GN",
    "FB", "HW", "HW?", "HR", "ES", "BT", "OM", "YL", "FER", "CALL", "CFM", "GL", "GD", "DR", "ABT",
    "CUL", "SRI", "NR", "TEST", "POTA", "SOTA", "UP", "?", "=",
};

static bool alpha(char c) { return c >= 'A' && c <= 'Z'; }
static bool digit(char c) { return c >= '0' && c <= '9'; }

static bool word_is(const char *s, size_t n, const char *t) {
    return strlen(t) == n && !strncmp(s, t, n);
}

static bool token(const char *s, size_t n) {
    for (size_t i = 0; i < sizeof(tokens) / sizeof(tokens[0]); i++)
        if (word_is(s, n, tokens[i]))
            return true;
    return false;
}

/* A name or a place follows */
static bool label(const char *s, size_t n) {
    return word_is(s, n, "NAME") || word_is(s, n, "NM") || word_is(s, n, "QTH") || word_is(s, n, "WX");
}

#define KNOWN       32

static char   known[KNOWN][16];
static size_t known_n, known_next;

bool cw_parse_known(const char *w, size_t n) {
    for (size_t i = 0; i < known_n; i++)
        if (word_is(w, n, known[i]))
            return true;
    return false;
}

const char *cw_parse_known_at(size_t i) {
    return i < known_n ? known[i] : NULL;
}

void cw_parse_forget(void) {
    known_n = known_next = 0;
}

/* s is a word of text */
static bool in_text(const char *text, const char *s, size_t n) {
    size_t l;

    for (const char *p = text; p && *p; p += l ? l : 1) {
        l = strcspn(p, " \n");
        if (l == n && !strncmp(p, s, n))
            return true;
    }
    return false;
}

/* <AR>, <SK>, <KN>, <CQ>: a prosign as the decoder writes it (not <?>, an unknown sign) */
static bool prosign(const char *s, size_t n) {
    if (n < 4 || s[0] != '<' || s[n - 1] != '>')
        return false;
    for (size_t i = 1; i < n - 1; i++)
        if (!alpha(s[i]))
            return false;
    return true;
}

/* 599, 5NN, 579 (N is a cut 9) */
static bool rst(const char *s, size_t n) {
    if (n != 3 || s[0] < '1' || s[0] > '5')
        return false;
    for (int i = 1; i < 3; i++)
        if (s[i] != 'N' && (s[i] < '1' || s[i] > '9'))
            return false;
    return true;
}

static bool number(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!digit(s[i]))
            return false;
    return n > 0;
}

/* Prefix, one digit, 1-4 letters: the prefix is 1-2 letters (I, IU), a letter and a digit (S5)
 * or a digit and 1-2 letters (2E, 9A, 3DA). Returns the suffix letters, 0 when not a call */
static size_t core_call(const char *s, size_t n) {
    size_t suf = 0, pl;

    while (suf < n && alpha(s[n - 1 - suf]))
        suf++;
    if (suf < 1 || suf > 4 || suf + 2 > n || !digit(s[n - suf - 1]))
        return 0;
    pl = n - suf - 1;
    if ((pl == 1 && alpha(s[0]))
        || (pl == 2 && alpha(s[0]) && (alpha(s[1]) || digit(s[1])))
        || (pl == 2 && digit(s[0]) && alpha(s[1]))
        || (pl == 3 && digit(s[0]) && alpha(s[1]) && alpha(s[2])))
        return suf;
    return 0;
}

/* /P /M /MM /QRP /AM /3 */
static bool post(const char *s, size_t n) {
    return (n == 1 && (s[0] == 'P' || s[0] == 'M' || digit(s[0])))
        || (n == 2 && (!strncmp(s, "MM", 2) || !strncmp(s, "AM", 2)))
        || (n == 3 && !strncmp(s, "QRP", 3));
}

/* EA8/ F/ VP2E/ */
static bool pre(const char *s, size_t n) {
    bool letter = false;

    if (n < 1 || n > 4)
        return false;
    for (size_t i = 0; i < n; i++) {
        if (alpha(s[i]))
            letter = true;
        else if (!digit(s[i]))
            return false;
    }
    return letter;
}

static size_t call_suffix(const char *s, size_t n) {
    const char *a = memchr(s, '/', n), *b;

    if (n < 3 || n > MAX_SEG)
        return 0;
    if (!a)
        return core_call(s, n);
    b = memchr(a + 1, '/', n - (a + 1 - s));
    if (b)
        return pre(s, a - s) && post(b + 1, s + n - b - 1) ? core_call(a + 1, b - a - 1) : 0;
    if (post(a + 1, s + n - a - 1))
        return core_call(s, a - s);
    return pre(s, a - s) ? core_call(a + 1, s + n - a - 1) : 0;
}

bool cw_parse_is_call(const char *w, size_t n) {
    return call_suffix(w, n) > 0;
}

bool cw_parse_know(const char *call, size_t n) {
    if (n >= sizeof(known[0]) || !cw_parse_is_call(call, n) || cw_parse_known(call, n))
        return false;
    memcpy(known[known_next], call, n);
    known[known_next][n] = '\0';
    known_next = (known_next + 1) % KNOWN;
    if (known_n < KNOWN)
        known_n++;
    return true;
}

cwp_class_t cw_parse_class(const char *w, size_t n) {
    if (prosign(w, n))
        return n == 5 && !strncmp(w, "<ERR>", 5) ? CWP_PLAIN : CWP_ABBR;
    if (token(w, n))
        return CWP_ABBR;
    if (rst(w, n))
        return CWP_RST;
    if (cw_parse_is_call(w, n))
        return CWP_CALL;
    return CWP_PLAIN;
}

/* What a piece costs as one word, 0 when it is none */
static int cost(const char *s, size_t n, const char *text) {
    size_t suf;

    if (token(s, n) || prosign(s, n) || rst(s, n))
        return 10;
    if (number(s, n))
        return 20;
    suf = call_suffix(s, n);
    if (suf)
        return in_text(text, s, n) || cw_parse_known(s, n) ? 8 : 22 + (suf == 4 ? 14 : 0) + (n == 3 ? 5 : 0);
    return 0;
}

/* The rest of a word: "GIOGIO" (sent twice) as two */
static char *put_rest(char *o, const char *s, size_t n) {
    if (n >= 6 && !(n & 1) && !memcmp(s, s + n / 2, n / 2)) {
        memcpy(o, s, n / 2);
        o[n / 2] = ' ';
        memcpy(o + n / 2 + 1, s, n / 2);
        return o + n + 1;
    }
    memcpy(o, s, n);
    return o + n;
}

size_t cw_parse_split(const char *w, size_t n, const char *text, char *out, size_t size) {
    int     best[MAX_WORD + 1], total;
    uint8_t from[MAX_WORD + 1];
    bool    lab[MAX_WORD + 1];
    size_t  cut = 0, k = 0, ends[MAX_WORD];
    char   *o = out;

    if (size < 2 * n + 1)
        return 0;
    if (n < 2 || n > MAX_WORD)
        goto plain;
    for (size_t i = 0; i <= n; i++) {
        best[i] = i ? INT32_MAX : 0;
        lab[i]  = false;
    }
    for (size_t i = 0; i < n; i++) {
        if (best[i] == INT32_MAX)
            continue;
        for (size_t l = 1; l <= MAX_SEG && i + l <= n; l++) {
            int c = cost(w + i, l, text);

            if (c && best[i] + c < best[i + l]) {
                best[i + l] = best[i] + c;
                from[i + l] = i;
                lab[i + l]  = label(w + i, l);
            }
        }
    }
    /* A label, then a name to the end of the word */
    total = best[n];
    for (size_t i = 1; i + 2 <= n; i++) {
        if (lab[i] && best[i] + FREE_REST < total) {
            total = best[i] + FREE_REST;
            cut   = i;
        }
    }
    if (total == INT32_MAX) {
        /* No reading: still, a word sent twice */
        o = put_rest(o, w, n);
        *o = '\0';
        return o - out;
    }
    for (size_t e = cut ? cut : n; e > 0; e = from[e])
        ends[k++] = e;
    if (k == 1 && !cut)
        goto plain;
    for (size_t s = 0; k > 0; k--) {
        memcpy(o, w + s, ends[k - 1] - s);
        o += ends[k - 1] - s;
        s  = ends[k - 1];
        if (k > 1 || cut)
            *o++ = ' ';
    }
    if (cut)
        o = put_rest(o, w + cut, n - cut);
    *o = '\0';
    return o - out;

plain:
    memcpy(out, w, n);
    out[n] = '\0';
    return n;
}

static bool is_de(const char *b, const char *e) {
    return e - b == 2 && b[0] == 'D' && b[1] == 'E';
}

bool cw_parse_confirmed(const char *text, const char *w, size_t n) {
    const char *b, *e, *p;
    size_t      l;
    int         times = 0;

    if (!cw_parse_is_call(w, n))
        return false;
    if (cw_parse_known(w, n))
        return true;
    for (e = w; e > text && (e[-1] == ' ' || e[-1] == '\n'); e--) {}
    for (b = e; b > text && b[-1] != ' ' && b[-1] != '\n'; b--) {}
    if (is_de(b, e))
        return true;
    for (b = w + n; *b == ' ' || *b == '\n'; b++) {}
    e = b + strcspn(b, " \n");
    if (*e && is_de(b, e))                  /* DE done, not the start of a longer word */
        return true;
    for (p = text; *p; p += l ? l : 1) {
        l = strcspn(p, " \n");
        if (l == n && !strncmp(p, w, n) && ++times >= 2)
            return true;
    }
    return false;
}
