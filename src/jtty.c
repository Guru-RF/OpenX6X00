/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  Xiegu X6100 LVGL GUI - JTTY protocol
 *
 *  C port of WSJT-X 3.2.0-rc1 lib/jtty/ (source grammar, text packer,
 *  CRC-12, tail-biting code and list decoder) and of the standard
 *  callsign packing of lib/77bit/packjt77.f90. Original Fortran:
 *  Copyright the WSJT Development Group, K1JT et al., GPL-3.0.
 *  See jtty.h. The normative description is lib/jtty/jtty_source_encoding.txt.
 */

#include "jtty.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const uint8_t jtty_sync[JTTY_NSYNC] = {0, 2, 2, 3, 0, 0, 3, 2, 1, 3, 1, 2, 0};

static const char ALPHABET[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ +-./?!\"#$%,&*()_'=[]{}<>|:;";

static const char *const CONTROL_TEXT[18] = {
    "AGN?", "CALL?", "AGN CALL", "NR?", "AGN NR", "EXCH?", "STATE?", "SECTION?", "ZONE?",
    "GRID?", "RPRT?", "QSL TU", "TU", "QRZ?", "QSO B4", "WAIT", "NIL?", "OK?",
};

/* PACK77_ARRL_SECTIONS, lib/77bit/packjt77_grammar.f90 */
static const char *const SECTIONS[86] = {
    "AB", "AK", "AL", "AR", "AZ", "BC", "CO", "CT", "DE", "EB", "EMA", "ENY", "EPA", "EWA", "GA",
    "GH", "IA", "ID", "IL", "IN", "KS", "KY", "LA", "LAX", "NS", "MB", "MDC", "ME", "MI", "MN",
    "MO", "MS", "MT", "NC", "ND", "NE", "NFL", "NH", "NL", "NLI", "NM", "NNJ", "NNY", "TER", "NTX",
    "NV", "OH", "OK", "ONE", "ONN", "ONS", "OR", "ORG", "PAC", "PR", "QC", "RI", "SB", "SC", "SCV",
    "SD", "SDG", "SF", "SFL", "SJV", "SK", "SNJ", "STX", "SV", "TN", "UT", "VA", "VI", "VT", "WCF",
    "WI", "WMA", "WNY", "WPA", "WTX", "WV", "WWA", "WY", "DX", "PE", "NB",
};

/* ---- Standard callsigns: pack28 / unpack28 / chkcall ------------------ */

#define NTOKENS 2063592
#define MAX22   4194304

static const char A1[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const char A2[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const char A3[] = "0123456789";
static const char A4[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ";

static int idx_of(const char *set, char c)
{
    const char *p = c ? strchr(set, c) : NULL;
    return p ? (int)(p - set) : -1;
}

/* pack77_c28_standard_shape */
static bool c28_shape(const char *s)
{
    int n = (int)strlen(s), iarea = 0, npdig = 0, nplet = 0;

    if (n < 3 || n > 6)
        return false;
    for (int i = 0; i < n; i++)
        if (!isupper((unsigned char)s[i]) && !isdigit((unsigned char)s[i]))
            return false;
    for (int i = n; i >= 2; i--)
        if (isdigit((unsigned char)s[i - 1])) {
            iarea = i;
            break;
        }
    if (iarea != 2 && iarea != 3)
        return false;
    for (int i = 0; i < iarea - 1; i++) {
        if (isdigit((unsigned char)s[i]))
            npdig++;
        else
            nplet++;
    }
    if (nplet == 0 || npdig >= iarea - 1)
        return false;
    for (int i = iarea; i < n; i++)
        if (!isupper((unsigned char)s[i]))
            return false;
    return n - iarea <= 3;
}

/* chkcall.f90, for calls without '/' (the only kind JTTY carries) */
static bool chkcall(const char *w)
{
    int n = (int)strlen(w), i1 = 0, ns = 0;

    if (n > 6 || strpbrk(w, ".+-?/"))
        return false;
    if (!isupper((unsigned char)w[0]) && !(n > 1 && isupper((unsigned char)w[1])))
        return false;
    if (w[0] == 'Q' && strncmp(w, "QU1RK", 5) != 0)
        return false;
    if (n > 1 && isdigit((unsigned char)w[1]))
        i1 = 2;
    if (n > 2 && isdigit((unsigned char)w[2]))
        i1 = 3;
    if (i1 == 0 || i1 == n)
        return false;
    for (int i = i1; i < n; i++) {
        if (!isupper((unsigned char)w[i]))
            return false;
        ns++;
    }
    return ns >= 1 && ns <= 3;
}

/* pack28 for a standard call; -1 if it is not one */
static int32_t pack28(const char *c)
{
    char cs[7];
    int  n = (int)strlen(c), iarea = 0;

    if (!c28_shape(c))
        return -1;
    for (int i = n; i >= 2; i--)
        if (isdigit((unsigned char)c[i - 1])) {
            iarea = i;
            break;
        }
    snprintf(cs, sizeof(cs), iarea == 2 ? " %-5s" : "%-6s", c);
    int i1 = idx_of(A1, cs[0]), i2 = idx_of(A2, cs[1]), i3 = idx_of(A3, cs[2]);
    int i4 = idx_of(A4, cs[3]), i5 = idx_of(A4, cs[4]), i6 = idx_of(A4, cs[5]);
    if (i1 < 0 || i2 < 0 || i3 < 0 || i4 < 0 || i5 < 0 || i6 < 0)
        return -1;
    return 36 * 10 * 27 * 27 * 27 * i1 + 10 * 27 * 27 * 27 * i2 + 27 * 27 * 27 * i3 + 27 * 27 * i4 +
           27 * i5 + i6 + NTOKENS + MAX22;
}

/* callok, packjt77.f90 */
static bool callok(const char *w)
{
    int n = (int)strlen(w), i0 = 0, nlp = 0;

    if (n < 3 || w[0] == 'Q')
        return false;
    for (int i = n; i >= 1; i--)
        if (isdigit((unsigned char)w[i - 1])) {
            i0 = i;
            break;
        }
    if (i0 != 2 && i0 != 3)
        return false;
    for (int i = 0; i < i0 - 1; i++) {
        if (isupper((unsigned char)w[i]))
            nlp++;
        else if (!isdigit((unsigned char)w[i]))
            return false;
    }
    if (nlp == 0 || n - i0 < 1 || n - i0 > 3)
        return false;
    for (int i = i0; i < n; i++)
        if (!isupper((unsigned char)w[i]))
            return false;
    return true;
}

/* unpack28, standard calls only */
static bool unpack28(int32_t n28, char out[14])
{
    char c[7];
    int  n;

    if (n28 < NTOKENS + MAX22)
        return false;
    n = n28 - NTOKENS - MAX22;
    c[0] = A1[n / (36 * 10 * 27 * 27 * 27)];
    n %= 36 * 10 * 27 * 27 * 27;
    c[1] = A2[n / (10 * 27 * 27 * 27)];
    n %= 10 * 27 * 27 * 27;
    c[2] = A3[n / (27 * 27 * 27)];
    n %= 27 * 27 * 27;
    c[3] = A4[n / (27 * 27)];
    n %= 27 * 27;
    c[4] = A4[n / 27];
    c[5] = A4[n % 27];
    c[6] = '\0';

    const char *p = c;
    while (*p == ' ')
        p++;
    snprintf(out, 14, "%s", p);
    for (int i = (int)strlen(out) - 1; i >= 0 && out[i] == ' '; i--)
        out[i] = '\0';
    if (strchr(out, ' ') || !callok(out))
        return false;
    return true;
}

/* jtty_source_codec standard_call: what a JTTY call atom can carry */
static bool standard_call(const char *s)
{
    char rt[14];
    int32_t n28;

    if (!chkcall(s))
        return false;
    for (const char *p = s; *p; p++)
        if (!isupper((unsigned char)*p) && !isdigit((unsigned char)*p))
            return false;
    n28 = pack28(s);
    return n28 >= 0 && unpack28(n28, rt) && strcmp(rt, s) == 0;
}

/* ---- Source atoms ------------------------------------------------------ */

static int source_index(char c)
{
    return idx_of(ALPHABET, c);
}

static bool valid_number(int kind, int v)
{
    if (kind < 0 || kind > 7 || v < 0 || v > 131071)
        return false;
    if (kind == 1)
        return v >= 1 && v <= 40;
    if (kind == 2)
        return v >= 1 && v <= 90;
    if (kind == 6)
        return v <= 9999;
    return true;
}

static bool pack_base36(const char *t, uint64_t *v)
{
    int n = (int)strlen(t);

    *v = 0;
    if (n != 2 && n != 3)
        return false;
    for (int i = 0; i < n; i++) {
        int d = idx_of(A2, t[i]);
        if (d < 0)
            return false;
        *v = 36 * *v + (uint64_t)d;
    }
    return !(n == 3 && *v < 36 * 36);
}

static void unpack_base36(uint64_t v, int n, char *t)
{
    for (int i = n - 1; i >= 0; i--) {
        t[i] = A2[v % 36];
        v /= 36;
    }
    t[n] = '\0';
}

static int section_index(const char *s)
{
    /* pack77_arrl_section_index looks at three characters: a two-letter
     * section given without its trailing blank is not found (as there) */
    if (strlen(s) < 3)
        return -1;
    for (int i = 0; i < 86; i++)
        if (strncmp(SECTIONS[i], s, 3) == 0 && strlen(SECTIONS[i]) == 3)
            return i + 1;
    return -1;
}

static uint64_t struct30(uint64_t body, int family)
{
    return 4 * (8 * body + (uint64_t)family) + 2;
}

static bool pack_atom(const jtty_atom_t *a, bool eom, uint64_t *frame)
{
    uint64_t n32 = 0, tok, body, pd;
    int n;

    switch (a->kind) {
    case JTTY_ATOM_CALL: {
        if (a->subtype < 0 || a->subtype > 5 || !standard_call(a->text))
            return false;
        int i2 = a->subtype <= 3 ? 0 : 1, n2 = a->subtype <= 3 ? a->subtype : a->subtype - 4;
        n32 = ((uint64_t)pack28(a->text) << 4) + 4 * (uint64_t)n2 + (uint64_t)i2;
        break;
    }
    case JTTY_ATOM_EXCH_NUM:
        if ((a->role != 0 && a->role != 1) || !valid_number(a->subtype, a->value))
            return false;
        body = ((uint64_t)a->role << 26) + ((uint64_t)a->subtype << 22) + ((uint64_t)a->value << 5);
        n32 = struct30(body, 0);
        break;
    case JTTY_ATOM_EXCH_LOC:
        n = (int)strlen(a->text);
        if ((a->role != 0 && a->role != 1) || a->subtype < 0 || a->subtype > 4 || !pack_base36(a->text, &tok))
            return false;
        body = ((uint64_t)a->role << 26) + ((uint64_t)a->subtype << 22) + ((uint64_t)(n - 2) << 21) + (tok << 5);
        n32 = struct30(body, 1);
        break;
    case JTTY_ATOM_EXCH_PAIR:
        if (a->subtype == 0) {
            n = (int)strlen(a->text);
            if (a->value < 1 || a->value > 40 || !pack_base36(a->text, &tok))
                return false;
            pd = ((uint64_t)a->value << 17) + ((uint64_t)(n - 2) << 16) + tok;
        } else if (a->subtype == 1) {
            int cl = a->text[0] - 'A';
            if (a->value < 1 || a->value > 32 || a->value2 < 1 || a->value2 > 86 || strlen(a->text) != 1 ||
                cl < 0 || cl > 5)
                return false;
            pd = ((uint64_t)a->value << 17) + ((uint64_t)cl << 14) + ((uint64_t)a->value2 << 7);
        } else {
            return false;
        }
        n32 = struct30(((uint64_t)a->subtype << 24) + (pd << 1), 2);
        break;
    case JTTY_ATOM_EXCH_NUM_TIME:
        if ((a->role != 0 && a->role != 1) || a->value < 0 || a->value > 16383 || a->value2 < 0 || a->value2 > 1439)
            return false;
        n32 = struct30(((uint64_t)a->role << 26) + ((uint64_t)a->value << 12) + ((uint64_t)a->value2 << 1), 3);
        break;
    case JTTY_ATOM_CONTROL:
        if (a->subtype < 0 || a->subtype > 17)
            return false;
        n32 = struct30((uint64_t)a->subtype << 16, 4);
        break;
    case JTTY_ATOM_GRID4: {
        const char *g = a->text;
        if ((a->role != 0 && a->role != 1) || strlen(g) != 4 || g[0] < 'A' || g[0] > 'R' || g[1] < 'A' ||
            g[1] > 'R' || !isdigit((unsigned char)g[2]) || !isdigit((unsigned char)g[3]))
            return false;
        int gi = (((g[0] - 'A') * 18 + (g[1] - 'A')) * 10 + (g[2] - '0')) * 10 + (g[3] - '0');
        n32 = struct30((1ull << 23) + ((uint64_t)a->role << 22) + ((uint64_t)gi << 7), 4);
        break;
    }
    case JTTY_ATOM_TEXT5: {
        uint64_t top = 0;
        size_t   len = strlen(a->text);
        for (size_t i = 0; i < 5; i++) {
            int k = source_index(i < len ? a->text[i] : ' ');     /* padded with spaces */
            if (k < 0)
                return false;
            top = 64 * top + (uint64_t)k;
        }
        n32 = 4 * top + 3;
        break;
    }
    default:
        return false;
    }
    *frame = (n32 << 2) | (eom ? 1 : 0);
    return true;
}

#define BITS(v, pos, len) ((int)(((v) >> (pos)) & ((1ull << (len)) - 1)))

bool jtty_unpack_frame(uint64_t frame, jtty_atom_t *a, bool *eom)
{
    uint64_t n32 = (frame >> 2) & 0xffffffffull, top = n32 >> 2, body;
    int i2 = (int)(n32 & 3);

    memset(a, 0, sizeof(*a));
    a->kind = -1;
    *eom = false;
    if (frame >> 34 || (frame & 2) || n32 == 0)
        return false;

    switch (i2) {
    case 0:
    case 1: {
        int n2 = BITS(n32, 2, 2);
        if (i2 == 1 && n2 > 1)
            return false;
        if (!unpack28((int32_t)(n32 >> 4), a->text) || !standard_call(a->text))
            return false;
        a->kind = JTTY_ATOM_CALL;
        a->subtype = i2 == 0 ? n2 : n2 + 4;
        break;
    }
    case 2: {
        int family = (int)(top & 7), n;
        uint64_t tok;
        body = top >> 3;
        switch (family) {
        case 0:
            a->kind = JTTY_ATOM_EXCH_NUM;
            a->role = BITS(body, 26, 1);
            a->subtype = BITS(body, 22, 4);
            a->value = BITS(body, 5, 17);
            if (BITS(body, 0, 5) || !valid_number(a->subtype, a->value))
                return false;
            break;
        case 1:
            a->kind = JTTY_ATOM_EXCH_LOC;
            a->role = BITS(body, 26, 1);
            a->subtype = BITS(body, 22, 4);
            n = BITS(body, 21, 1) + 2;
            tok = (uint64_t)BITS(body, 5, 16);
            if (BITS(body, 0, 5) || a->subtype > 4 || tok >= (n == 2 ? 36u * 36 : 36u * 36 * 36) ||
                (n == 3 && tok < 36 * 36))
                return false;
            unpack_base36(tok, n, a->text);
            break;
        case 2: {
            uint64_t pd = (body >> 1) & ((1u << 23) - 1);
            a->kind = JTTY_ATOM_EXCH_PAIR;
            a->subtype = BITS(body, 24, 3);
            if (body & 1)
                return false;
            if (a->subtype == 0) {
                a->value = BITS(pd, 17, 6);
                n = BITS(pd, 16, 1) + 2;
                tok = (uint64_t)BITS(pd, 0, 16);
                if (a->value < 1 || a->value > 40 || tok >= (n == 2 ? 36u * 36 : 36u * 36 * 36) ||
                    (n == 3 && tok < 36 * 36))
                    return false;
                unpack_base36(tok, n, a->text);
            } else if (a->subtype == 1) {
                int cl = BITS(pd, 14, 3);
                a->value = BITS(pd, 17, 6);
                a->value2 = BITS(pd, 7, 7);
                if (a->value < 1 || a->value > 32 || cl > 5 || a->value2 < 1 || a->value2 > 86 || BITS(pd, 0, 7))
                    return false;
                a->text[0] = (char)('A' + cl);
            } else {
                return false;
            }
            break;
        }
        case 3:
            a->kind = JTTY_ATOM_EXCH_NUM_TIME;
            a->role = BITS(body, 26, 1);
            a->value = BITS(body, 12, 14);
            a->value2 = BITS(body, 1, 11);
            if ((body & 1) || a->value2 > 1439)
                return false;
            break;
        case 4: {
            int sub = BITS(body, 23, 4);
            uint64_t d = body & ((1u << 23) - 1);
            if (sub == 0) {
                a->kind = JTTY_ATOM_CONTROL;
                a->subtype = BITS(d, 16, 7);
                if (BITS(d, 0, 16) || a->subtype > 17)
                    return false;
            } else if (sub == 1) {
                int gi = BITS(d, 7, 15);
                a->kind = JTTY_ATOM_GRID4;
                a->role = BITS(d, 22, 1);
                if (BITS(d, 0, 7) || gi >= 32400)
                    return false;
                a->text[3] = (char)('0' + gi % 10);
                gi /= 10;
                a->text[2] = (char)('0' + gi % 10);
                gi /= 10;
                a->text[1] = (char)('A' + gi % 18);
                a->text[0] = (char)('A' + gi / 18);
            } else {
                return false;
            }
            break;
        }
        default:
            return false;
        }
        break;
    }
    case 3:
        a->kind = JTTY_ATOM_TEXT5;
        for (int i = 0; i < 5; i++)
            a->text[i] = ALPHABET[(top >> (6 * (4 - i))) & 63];
        break;
    }
    *eom = frame & 1;
    return true;
}

static void render_number(int kind, int v, char *t, size_t sz)
{
    if (kind == 0 && v < 1000)
        snprintf(t, sz, "%03d", v);
    else if ((kind == 1 || kind == 2 || kind == 5) && v < 100)
        snprintf(t, sz, "%02d", v);
    else if (kind == 6)
        snprintf(t, sz, "%04d", v);
    else
        snprintf(t, sz, "%d", v);
}

bool jtty_render_atom(const jtty_atom_t *a, char *out)
{
    char f[24];
    const char *pfx = a->role ? "599 " : "";

    out[0] = '\0';
    switch (a->kind) {
    case JTTY_ATOM_CALL: {
        static const char *const fmt[6] = {"CQ %s CQ", "%s", "TU %s CQ", "%s TU", "%s AGN?", "TU NOW %s"};
        if (a->subtype < 0 || a->subtype > 5)
            return false;
        snprintf(out, 81, fmt[a->subtype], a->text);
        return true;
    }
    case JTTY_ATOM_EXCH_NUM:
        if (!valid_number(a->subtype, a->value))
            return false;
        render_number(a->subtype, a->value, f, sizeof(f));
        snprintf(out, 81, "%s%s", pfx, f);
        return true;
    case JTTY_ATOM_EXCH_LOC:
        snprintf(out, 81, "%s%s", pfx, a->text);
        return true;
    case JTTY_ATOM_EXCH_PAIR:
        if (a->subtype == 0)
            snprintf(out, 81, "599 %02d %s", a->value, a->text);
        else if (a->value2 >= 1 && a->value2 <= 86)
            snprintf(out, 81, "%d%c %s", a->value, a->text[0], SECTIONS[a->value2 - 1]);
        else
            return false;
        return true;
    case JTTY_ATOM_EXCH_NUM_TIME:
        render_number(0, a->value, f, sizeof(f));
        snprintf(out, 81, "%s%s %02d%02d", pfx, f, a->value2 / 60, a->value2 % 60);
        return true;
    case JTTY_ATOM_CONTROL:
        snprintf(out, 81, "%s", CONTROL_TEXT[a->subtype]);
        return true;
    case JTTY_ATOM_GRID4:
        snprintf(out, 81, "%s%s", pfx, a->text);
        return true;
    case JTTY_ATOM_TEXT5:
        snprintf(out, 81, "%.5s", a->text);
        return true;
    }
    return false;
}

/* ---- Text packer (pack_jtty) ------------------------------------------- */

static void normalize(const char *raw, char *out)
{
    bool last_space = true;
    int  j = 0;

    for (int i = 0; raw[i] && i < JTTY_MSG_LEN; i++) {
        char c = raw[i];
        if (c == '~')
            c = ' ';
        c = (char)toupper((unsigned char)c);
        if (source_index(c) < 0)
            c = '#';
        if (c == ' ') {
            if (last_space)
                continue;
            last_space = true;
        } else {
            last_space = false;
        }
        out[j++] = c;
    }
    while (j > 0 && out[j - 1] == ' ')
        j--;
    out[j] = '\0';
}

static bool decimal_value(const char *t, int len, int *v)
{
    *v = 0;
    if (len < 1 || len > 6)
        return false;
    for (int i = 0; i < len; i++) {
        if (!isdigit((unsigned char)t[i]))
            return false;
        *v = 10 * *v + (t[i] - '0');
    }
    return *v <= 131071;
}

/* RTTY Roundup: a number after a whole "599" token becomes a serial */
static bool normalize_serials(char *msg)
{
    char res[256] = "", tok[90];
    const char *p = msg;

    while (*p) {
        const char *e = strchr(p, ' ');
        int len = e ? (int)(e - p) : (int)strlen(p), v;
        const char *next = e ? e + 1 : p + len;

        snprintf(tok, sizeof(tok), "%.*s", len, p);
        if (strcmp(tok, "599") == 0 && *next) {
            const char *e2 = strchr(next, ' ');
            int len2 = e2 ? (int)(e2 - next) : (int)strlen(next);
            if (decimal_value(next, len2, &v)) {
                snprintf(tok, sizeof(tok), v < 1000 ? "599 %03d" : "599 %d", v);
                next = e2 ? e2 + 1 : next + len2;
            }
        }
        if (res[0])
            strcat(res, " ");
        strcat(res, tok);
        if (strlen(res) > JTTY_MSG_LEN)
            return false;
        p = next;
    }
    strcpy(msg, res);
    return true;
}

typedef struct {
    const char *msg;
    int n, ipos, profile;
    int dp[JTTY_MSG_LEN + 2], succ[JTTY_MSG_LEN + 2];
    jtty_atom_t choice[JTTY_MSG_LEN + 2];
} packer_t;

static void consider(packer_t *P, const jtty_atom_t *a, int next)
{
    int i = P->ipos, cost = 1 + P->dp[next];
    const jtty_atom_t *b = &P->choice[i];

    if (cost > JTTY_MAX_FRAMES || cost > P->dp[i])
        return;
    if (cost == P->dp[i]) {
        int rank = a->kind == JTTY_ATOM_TEXT5, best = b->kind == JTTY_ATOM_TEXT5;
        int key = 100 * a->kind + 2 * a->subtype + a->role, bkey = 100 * b->kind + 2 * b->subtype + b->role;
        if (rank > best)
            return;
        if (rank == best) {
            if (next < P->succ[i])
                return;
            if (next == P->succ[i] && key >= bkey)
                return;
        }
    }
    P->dp[i] = cost;
    P->succ[i] = next;
    P->choice[i] = *a;
}

static void offer(packer_t *P, const jtty_atom_t *a)
{
    uint64_t f;
    jtty_atom_t d;
    bool eom;
    char r[81];
    int len, last, next;

    if (!pack_atom(a, false, &f) || !jtty_unpack_frame(f, &d, &eom) || !jtty_render_atom(&d, r))
        return;
    len = (int)strlen(r);
    if (len == 0)
        return;
    last = P->ipos + len - 1;        /* 1-based, as in the Fortran */
    if (last > P->n || strncmp(P->msg + P->ipos - 1, r, (size_t)len) != 0)
        return;
    next = P->n + 1;
    if (last < P->n) {
        if (P->msg[last] != ' ')
            return;
        next = last + 2;
    }
    consider(P, a, next);
}

static jtty_atom_t mk(int kind, int sub, int role, int v, int v2, const char *t)
{
    jtty_atom_t a = {kind, sub, role, v, v2, ""};
    if (t)
        snprintf(a.text, sizeof(a.text), "%s", t);
    return a;
}

static void try_compact(packer_t *P)
{
    char words[3][81] = {"", "", ""};
    const char *p = P->msg + P->ipos - 1;
    int nw = 0, v, len;

    for (int j = 0; j < 3 && *p; j++) {
        const char *e = strchr(p, ' ');
        len = e ? (int)(e - p) : (int)strlen(p);
        snprintf(words[j], sizeof(words[j]), "%.*s", len, p);
        nw = j + 1;
        p = e ? e + 1 : p + len;
    }
    for (int j = 0; j < nw; j++) {
        if (strlen(words[j]) > 13)
            continue;
        for (int act = JTTY_CALL_CQ; act <= JTTY_CALL_TU_NOW; act++) {
            jtty_atom_t a = mk(JTTY_ATOM_CALL, act, 0, 0, 0, words[j]);
            offer(P, &a);
        }
    }
    for (int j = 0; j < 18; j++) {
        jtty_atom_t a = mk(JTTY_ATOM_CONTROL, j, 0, 0, 0, NULL);
        offer(P, &a);
    }
    for (int role = 0; role <= 1; role++) {
        int field = 0;
        if (role == 1) {
            if (strcmp(words[0], "599") != 0 || nw < 2)
                continue;
            field = 1;
        }
        const char *w = words[field];
        len = (int)strlen(w);
        bool num = decimal_value(w, len, &v);
        jtty_atom_t a;
        if (num) {
            a = mk(JTTY_ATOM_EXCH_NUM, 7, role, v, 0, NULL);
            offer(P, &a);
            if (role == 1 && P->profile == JTTY_PROFILE_RTTY) {
                a = mk(JTTY_ATOM_EXCH_NUM, 0, role, v, 0, NULL);
                offer(P, &a);
            }
        }
        if (len == 4) {
            a = mk(JTTY_ATOM_GRID4, 0, role, 0, 0, w);
            offer(P, &a);
        }
        if (role != 1 || len < 2 || len > 3 || !strpbrk(w, "ABCDEFGHIJKLMNOPQRSTUVWXYZ"))
            continue;
        a = mk(JTTY_ATOM_EXCH_LOC, 3, role, 0, 0, w);
        offer(P, &a);
        if (P->profile == JTTY_PROFILE_RTTY) {
            a = mk(JTTY_ATOM_EXCH_LOC, 0, role, 0, 0, w);
            offer(P, &a);
        }
    }
    len = (int)strlen(words[0]);
    if (nw < 2 || len < 2 || len > 3 || !decimal_value(words[0], len - 1, &v))
        return;
    char cl[2] = {words[0][len - 1], 0};
    jtty_atom_t a = mk(JTTY_ATOM_EXCH_PAIR, 1, 0, v, section_index(words[1]), cl);
    offer(P, &a);
}

int jtty_pack_text(const char *text, int profile, uint64_t frames[JTTY_MAX_FRAMES], char *canon)
{
    static packer_t P;      /* UI thread only; too big for a small stack */
    char msg[JTTY_MSG_LEN + 1];
    int n, nf = 0;

    normalize(text ? text : "", msg);
    if (profile == JTTY_PROFILE_RTTY && !normalize_serials(msg))
        return -1;
    if (canon)
        snprintf(canon, JTTY_MSG_LEN + 1, "%s", msg);
    n = (int)strlen(msg);
    if (n == 0)
        return 0;

    memset(&P, 0, sizeof(P));
    P.msg = msg;
    P.n = n;
    P.profile = profile;
    for (int i = 0; i <= n + 1; i++) {
        P.dp[i] = 999;
        P.choice[i].kind = -1;
    }
    P.dp[n + 1] = 0;
    for (P.ipos = n; P.ipos >= 1; P.ipos--) {
        int next = P.ipos + 5 < n + 1 ? P.ipos + 5 : n + 1;
        jtty_atom_t a = mk(JTTY_ATOM_TEXT5, 0, 0, 0, 0, NULL);
        memcpy(a.text, msg + P.ipos - 1, (size_t)(next - P.ipos));
        a.text[next - P.ipos] = '\0';
        consider(&P, &a, next);
        if (P.ipos > 1 && msg[P.ipos - 2] != ' ')
            continue;
        try_compact(&P);
    }
    if (P.dp[1] > JTTY_MAX_FRAMES)
        return -1;
    for (int i = 1; i <= n; i = P.succ[i]) {
        if (!pack_atom(&P.choice[i], P.succ[i] > n, &frames[nf]))
            return -1;
        nf++;
    }
    return nf;
}

/* ---- CRC-12 and the tail-biting code (tbcc.f90) ------------------------ */

#define NU      9
#define NSTATE  512
#define G0      0x277       /* 1167 octal */
#define G1      0x365       /* 1545 octal */
#define NINFO   46
#define NPAY    34

static int parity(unsigned v)
{
    return __builtin_parity(v);
}

static unsigned crc12(const uint8_t *bits, int n)
{
    unsigned crc = 0;

    for (int i = 0; i < n; i++) {
        crc ^= (unsigned)bits[i] << 11;
        crc = (crc & 0x800) ? (crc << 1) ^ 0x80f : crc << 1;
        crc &= 0xfff;
    }
    return crc;
}

static int trans_tone(int state, int bit, int *next)
{
    unsigned reg = (((unsigned)state << 1) | (unsigned)bit) & 0x3ff;
    int b0 = parity(reg & G0), b1 = parity(reg & G1);

    *next = (int)(reg & (NSTATE - 1));
    return 2 * b0 + (b0 ^ b1);     /* Gray: 00 01 11 10 -> 0 1 2 3 */
}

void jtty_frame_tones(uint64_t frame, uint8_t tones[JTTY_NSYM])
{
    uint8_t info[NINFO];
    unsigned crc;
    int state = 0;

    for (int i = 0; i < NPAY; i++)
        info[i] = (frame >> (NPAY - 1 - i)) & 1;
    crc = crc12(info, NPAY);
    for (int i = 0; i < 12; i++)
        info[NPAY + i] = (crc >> (11 - i)) & 1;

    for (int i = NINFO - NU; i < NINFO; i++)
        state = ((state << 1) | info[i]) & (NSTATE - 1);
    memcpy(tones, jtty_sync, JTTY_NSYNC);
    for (int t = 0; t < NINFO; t++)
        tones[JTTY_NSYNC + t] = (uint8_t)trans_tone(state, info[t], &state);
}

/* ---- List decoder (jtty_tbcc_list_decoder.f90, P4/H4, two wraps) ------- */

#define WIDTH       4
#define HYP         4
#define RESERVED    33          /* 1-based info bit, always 0 */
#define ORIGIN_SH   46
#define VALID_BIT   (1ull << 57)
#define ID_MASK     ((1ull << 46) - 1)

typedef struct {
    double   metric;
    uint64_t key;       /* info bits (bit k at 46-k), origin state, valid */
} surv_t;

/* tone word (base 4, first symbol most significant) for len input bits
 * from a state; len 1, 2 and 4 */
static uint8_t tw1[NSTATE][2], tw2[NSTATE][4], tw4[NSTATE][16];
static bool    tables_ok = false;

static void init_tables(void)
{
    for (int s = 0; s < NSTATE; s++)
        for (int w = 0; w < 16; w++)
            for (int len = 1; len <= 4; len <<= 1) {
                if (w >= (1 << len))
                    continue;
                int st = s, word = 0;
                for (int o = 0; o < len; o++) {
                    int bit = (w >> (len - 1 - o)) & 1;
                    word = 4 * word + trans_tone(st, bit, &st);
                }
                if (len == 1)
                    tw1[s][w] = (uint8_t)word;
                else if (len == 2)
                    tw2[s][w] = (uint8_t)word;
                else
                    tw4[s][w] = (uint8_t)word;
            }
    tables_ok = true;
}

static inline int tone_word(int len, int s, int w)
{
    return len == 1 ? tw1[s][w] : len == 2 ? tw2[s][w] : tw4[s][w];
}

static inline bool precedes(const surv_t *a, const surv_t *b)
{
    if (a->metric != b->metric)
        return a->metric > b->metric;
    return a->key < b->key;
}

static void insert_surv(surv_t sel[WIDTH], const surv_t *c, bool dedup)
{
    int slot, ins;

    if (dedup) {
        for (slot = 0; slot < WIDTH; slot++) {
            if (!(sel[slot].key & VALID_BIT) || sel[slot].key != c->key)
                continue;
            if (c->metric <= sel[slot].metric)
                return;
            for (int i = slot; i < WIDTH - 1; i++)
                sel[i] = sel[i + 1];
            sel[WIDTH - 1].key = 0;
            break;
        }
    }
    ins = WIDTH;
    for (slot = 0; slot < WIDTH; slot++)
        if (!(sel[slot].key & VALID_BIT) || precedes(c, &sel[slot])) {
            ins = slot;
            break;
        }
    if (ins >= WIDTH)
        return;
    for (slot = WIDTH - 1; slot > ins; slot--)
        sel[slot] = sel[slot - 1];
    sel[ins] = *c;
}

typedef struct {
    uint64_t id;
    int      start;
    double   wava, clean;
    bool     crc;
} pool_t;

static surv_t prev_s[NSTATE][WIDTH], cur_s[NSTATE][WIDTH];
static pool_t pool[NSTATE * WIDTH];
static int    hash_ix[4096];
static double energies[46][256];

static double score_identity(int L, uint64_t id, bool *closed)
{
    int start = 0, state;
    double m = 0;

    for (int k = NINFO - NU + 1; k <= NINFO; k++)
        start = ((start << 1) | (int)((id >> (k - 1)) & 1)) & (NSTATE - 1);
    state = start;
    for (int b = 0, s0 = 0; s0 < NINFO; b++, s0 += L) {
        int len = NINFO - s0 < L ? NINFO - s0 : L, w = 0;
        for (int o = 0; o < len; o++)
            w = 2 * w + (int)((id >> (s0 + o)) & 1);
        m += energies[b][tone_word(len, state, w)];
        state = ((state << len) | w) & (NSTATE - 1);
    }
    *closed = state == start;
    return m;
}

static bool pool_before(const pool_t *a, const pool_t *b)
{
    if (a->clean != b->clean)
        return a->clean > b->clean;
    if (a->id != b->id)
        return a->id < b->id;
    if (a->start != b->start)
        return a->start < b->start;
    return a->wava > b->wava;
}

/* One rung: list Viterbi coherent over L symbols. True with a frame. */
static bool decode_rung(const float complex c[JTTY_NDATA][4], int L, uint64_t *frame)
{
    int nblk = 0, npool = 0;

    /* Coherent energy of every tone sequence of every block */
    for (int s0 = 0; s0 < NINFO; s0 += L, nblk++) {
        int len = NINFO - s0 < L ? NINFO - s0 : L;
        for (int w = 0; w < (1 << (2 * len)); w++) {
            float complex sum = 0;
            for (int o = 0; o < len; o++)
                sum += c[s0 + o][(w >> (2 * (len - 1 - o))) & 3];
            energies[nblk][w] = (double)(crealf(sum) * crealf(sum) + cimagf(sum) * cimagf(sum)) / len;
        }
    }

    memset(prev_s, 0, sizeof(prev_s));
    for (int s = 0; s < NSTATE; s++) {
        prev_s[s][0].key = ((uint64_t)s << ORIGIN_SH) | VALID_BIT;
        prev_s[s][0].metric = 0;
    }

    for (int wrap = 0; wrap < 2; wrap++) {
        for (int s = 0; s < NSTATE; s++)
            for (int r = 0; r < WIDTH; r++)
                if (prev_s[s][r].key & VALID_BIT)
                    prev_s[s][r].key = ((uint64_t)s << ORIGIN_SH) | VALID_BIT;
        for (int b = 0, s0 = 0; s0 < NINFO; b++, s0 += L) {
            int len = NINFO - s0 < L ? NINFO - s0 : L, bc = 1 << len, stride = NSTATE / bc;
            bool prune = s0 + 1 <= RESERVED && s0 + 1 + len > RESERVED;
            for (int e = 0; e < NSTATE; e++) {
                surv_t *sel = cur_s[e];
                int w = e & (bc - 1);
                uint64_t bid = 0;
                for (int o = 1; o <= len; o++)
                    if ((w >> (len - o)) & 1)
                        bid |= 1ull << (NINFO - s0 - o);
                for (int r = 0; r < WIDTH; r++)
                    sel[r].key = 0;
                if (prune && (bid >> (NINFO - RESERVED)) & 1)
                    continue;
                for (int st = e / bc; st < NSTATE; st += stride) {
                    double bm = energies[b][tone_word(len, st, w)];
                    for (int r = 0; r < WIDTH; r++) {
                        const surv_t *p = &prev_s[st][r];
                        if (!(p->key & VALID_BIT))
                            continue;
                        surv_t x = {p->metric + bm, p->key | bid};
                        if ((sel[WIDTH - 1].key & VALID_BIT) && x.metric < sel[WIDTH - 1].metric)
                            break;
                        insert_surv(sel, &x, b == 0);
                    }
                }
            }
            memcpy(prev_s, cur_s, sizeof(prev_s));
        }
    }

    /* Closed paths, one per distinct word, scored on this frame alone */
    memset(hash_ix, -1, sizeof(hash_ix));
    for (int s = 0; s < NSTATE; s++)
        for (int r = 0; r < WIDTH; r++) {
            const surv_t *p = &prev_s[s][r];
            uint64_t id = 0, k = p->key & ID_MASK;
            int h;
            if (!(p->key & VALID_BIT) || (int)((p->key >> ORIGIN_SH) & 0x7ff) != s)
                continue;
            for (int i = 0; i < NINFO; i++)
                if ((k >> i) & 1)
                    id |= 1ull << (NINFO - 1 - i);
            for (h = (int)(id % 4096); hash_ix[h] >= 0 && pool[hash_ix[h]].id != id; h = (h + 1) % 4096)
                ;
            if (hash_ix[h] >= 0) {
                pool_t *q = &pool[hash_ix[h]];
                if (p->metric > q->wava || (!(p->metric < q->wava) && s < q->start)) {
                    q->wava = p->metric;
                    q->start = s;
                }
                continue;
            }
            hash_ix[h] = npool;
            pool_t *q = &pool[npool++];
            bool closed;
            uint8_t bits[NINFO];
            q->id = id;
            q->start = s;
            q->wava = p->metric;
            q->clean = score_identity(L, id, &closed);
            for (int i = 0; i < NINFO; i++)
                bits[i] = (id >> i) & 1;
            q->crc = crc12(bits, NINFO) == 0;
        }

    /* The best HYP by clean metric go to the CRC */
    for (int rank = 0; rank < HYP && rank < npool; rank++) {
        int best = rank;
        for (int i = rank + 1; i < npool; i++)
            if (pool_before(&pool[i], &pool[best]))
                best = i;
        pool_t t = pool[rank];
        pool[rank] = pool[best];
        pool[best] = t;
        if (!pool[rank].crc)
            continue;
        uint64_t f = 0;
        for (int i = 0; i < NPAY; i++)
            f = (f << 1) | ((pool[rank].id >> i) & 1);
        if ((f >> 2) == 0 && (f & 3) == 0)
            return false;           /* the all-zero sentinel */
        *frame = f;
        return true;
    }
    return false;
}

bool jtty_decode_frame(const float complex corr[JTTY_NDATA][4],
                       const float complex half[JTTY_NDATA][4], uint64_t *frame)
{
    if (!tables_ok)
        init_tables();
    return decode_rung(corr, 1, frame) || decode_rung(corr, 2, frame) || decode_rung(corr, 4, frame) ||
           decode_rung(half, 1, frame);
}
