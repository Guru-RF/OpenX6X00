/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - GhostNet: the settings file, the schedule, the groups and the @GSTFLASH test (see ghostnet.h). Nothing
 *  here draws or touches the radio, so tools/ghostnet checks it on a PC.
 *
 *  After the GhostNet mode of the CX8ANM edition of OpenX6X00 (condoruy, Cóndor Tecnológico).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE     /* strcasestr, tm_gmtoff */
#endif
#include "ghostnet.h"
#include "i18n.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- The settings ---- */

static ghostnet_conf_t conf;
static time_t          conf_mtime = -1;     /* -1: not read yet */
static ino_t           conf_ino;

void ghostnet_conf_defaults(ghostnet_conf_t *c) {
    memset(c, 0, sizeof(*c));
    c->remind     = false;      /* nothing comes up until the operator turns it on */
    c->remind_min = 10;
    snprintf(c->own_name, sizeof(c->own_name), "Own net");
    c->own_day = 4;             /* Thursday, 21:00 local, as GhostNet's own nets */
    c->own_hh  = 21;
    c->own_min = 30;
    c->own_khz = 7107;
    snprintf(c->own_mode, sizeof(c->own_mode), "JS8");
}

/* Letters and digits only, in capitals, at most size - 1 */
static void alnum_upper(char *out, size_t size, const char *in) {
    size_t k = 0;

    for (; *in && k + 1 < size; in++) {
        if (isalnum((unsigned char)*in)) out[k++] = (char)toupper((unsigned char)*in);
    }
    out[k] = 0;
}

void ghostnet_conf_parse_line(ghostnet_conf_t *c, char *line) {
    char *eq = strchr(line, '='), *v;
    int   n;

    if (line[0] == '#' || !eq) return;
    *eq = 0;
    v = eq + 1;
    v[strcspn(v, "\r\n")] = 0;
    n = atoi(v);
    if (!strcmp(line, "remind")) {
        c->remind = n != 0;
    } else if (!strcmp(line, "remind_min")) {
        c->remind_min = n >= 1 && n <= 120 ? n : 10;
    } else if (!strcmp(line, "nets")) {
        snprintf(c->nets, sizeof(c->nets), "%s", v);
    } else if (!strcmp(line, "own_on")) {
        c->own_on = n != 0;
    } else if (!strcmp(line, "own_name")) {
        if (*v) snprintf(c->own_name, sizeof(c->own_name), "%s", v);
    } else if (!strcmp(line, "own_day")) {
        c->own_day = n >= 0 && n <= 6 ? n : 4;
    } else if (!strcmp(line, "own_time")) {
        int hh, mm;
        if (sscanf(v, "%d:%d", &hh, &mm) == 2 && hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59) {
            c->own_hh = hh;
            c->own_mm = mm;
        }
    } else if (!strcmp(line, "own_min")) {
        c->own_min = n >= 5 && n <= 240 ? n : 30;
    } else if (!strcmp(line, "own_khz")) {
        c->own_khz = n >= 1800 && n <= 54000 ? n : 7107;
    } else if (!strcmp(line, "own_mode")) {
        if (!strcmp(v, "JS8") || !strcmp(v, "RTTY") || !strcmp(v, "LSB") || !strcmp(v, "USB"))
            snprintf(c->own_mode, sizeof(c->own_mode), "%s", v);
    } else if (!strcmp(line, "dept")) {
        alnum_upper(c->dept, sizeof(c->dept), v);
    } else if (!strcmp(line, "region")) {
        snprintf(c->region, sizeof(c->region), "%s", v);
    } else if (!strcmp(line, "translate_rx")) {
        c->translate_rx = n != 0;
    } else if (!strcmp(line, "translate_tx")) {
        c->translate_tx = n != 0;
    }
}

const ghostnet_conf_t *ghostnet_conf(void) {
    struct stat st;
    FILE       *f;
    char        line[160];

    if (stat(GHOSTNET_CONF, &st) != 0) {
        if (conf_mtime != 0) ghostnet_conf_defaults(&conf);
        conf_mtime = 0;
        return &conf;
    }
    /* The web page puts a new file in place: a new inode, also within the same 2 s of FAT's time */
    if (st.st_mtime == conf_mtime && st.st_ino == conf_ino) return &conf;
    conf_mtime = st.st_mtime;
    conf_ino   = st.st_ino;
    ghostnet_conf_defaults(&conf);
    f = fopen(GHOSTNET_CONF, "r");
    if (!f) return &conf;
    while (fgets(line, sizeof(line), f)) ghostnet_conf_parse_line(&conf, line);
    fclose(f);
    return &conf;
}

bool ghostnet_conf_save(const ghostnet_conf_t *c) {
    char  tmp[sizeof(GHOSTNET_CONF) + 8];
    FILE *f;

    snprintf(tmp, sizeof(tmp), "%s.tmp", GHOSTNET_CONF);
    f = fopen(tmp, "w");
    if (!f) return false;
    fprintf(f, "# GhostNet: the JS8 app's GhostNet popup and the web page GhostNet write this file\n");
    fprintf(f, "remind=%d\nremind_min=%d\nnets=%s\n", c->remind, c->remind_min, c->nets);
    fprintf(f, "own_on=%d\nown_name=%s\nown_day=%d\nown_time=%02d:%02d\nown_min=%d\nown_khz=%d\nown_mode=%s\n",
            c->own_on, c->own_name, c->own_day, c->own_hh, c->own_mm, c->own_min, c->own_khz, c->own_mode);
    fprintf(f, "dept=%s\nregion=%s\n", c->dept, c->region);
    fprintf(f, "translate_rx=%d\ntranslate_tx=%d\n", c->translate_rx, c->translate_tx);
    fflush(f);
    fsync(fileno(f));           /* /mnt is FAT on the card */
    if (fclose(f) != 0 || rename(tmp, GHOSTNET_CONF) != 0) {
        unlink(tmp);
        return false;
    }
    conf       = *c;
    conf_mtime = -1;            /* read again: the new file's time and inode */
    ghostnet_conf();
    return true;
}

bool ghostnet_net_reminded(const ghostnet_conf_t *c, const char *net) {
    char list[sizeof(c->nets) + 2], want[16];

    snprintf(list, sizeof(list), ",%s,", c->nets);
    snprintf(want, sizeof(want), ",%s,", net);
    return strstr(list, want) != NULL;
}

void ghostnet_net_toggle(ghostnet_conf_t *c, const char *net) {
    char   out[sizeof(c->nets)] = "", list[sizeof(c->nets)];
    bool   had = false;
    char  *save = NULL;
    size_t len = 0;

    snprintf(list, sizeof(list), "%s", c->nets);
    for (char *id = strtok_r(list, ",", &save); id; id = strtok_r(NULL, ",", &save)) {
        if (!strcmp(id, net)) {
            had = true;         /* left out */
            continue;
        }
        len += snprintf(out + len, sizeof(out) - len, "%s%s", len ? "," : "", id);
        if (len >= sizeof(out)) return;     /* would not fit: as it was */
    }
    if (!had) {
        len += snprintf(out + len, sizeof(out) - len, "%s%s", len ? "," : "", net);
        if (len >= sizeof(out)) return;
    }
    snprintf(c->nets, sizeof(c->nets), "%s", out);
}

const char *ghostnet_region_net(int iaru_region) {
    switch (iaru_region) {
        case 2:  return "NA";
        case 3:  return "AU";
        default: return "EU";
    }
}

/* ---- The schedule ---- */

static const char *const day_names[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

bool ghostnet_sched_parse_line(ghostnet_sched_t *s, char *line) {
    char *f[6], *p = line, *rest, *end;
    int   n = 0;

    line[strcspn(line, "\r\n#")] = 0;
    /* The first words one by one (two of a net's line, six of a window's); the rest of the line is the net's name
     * or the window's description, spaces and all */
    while (n < 6) {
        p += strspn(p, " \t");
        if (!*p) break;
        f[n++] = p;
        p += strcspn(p, " \t");
        if (*p) *p++ = 0;
        if (n == 2 && !strcmp(f[0], "net")) break;
    }
    if (n == 0) return false;
    rest = p + strspn(p, " \t");
    for (end = rest + strlen(rest); end > rest && (end[-1] == ' ' || end[-1] == '\t');) *--end = 0;

    if (!strcmp(f[0], "net")) {
        if (n < 2 || s->nnets >= GHOSTNET_NETS_MAX) return false;
        ghostnet_net_t *net = &s->nets[s->nnets++];
        snprintf(net->id, sizeof(net->id), "%s", f[1]);
        snprintf(net->name, sizeof(net->name), "%s", *rest ? rest : f[1]);
        return true;
    }
    if (n < 6 || s->nsegs >= GHOSTNET_SEGS_MAX) return false;
    ghostnet_seg_t g = {0};
    int            wday = -1;
    for (int d = 0; d < 7; d++) {
        if (!strcasecmp(f[1], day_names[d])) wday = d;
    }
    long minutes = strtol(f[3], &end, 10);
    if (wday < 0 || sscanf(f[2], "%d:%d", &g.hh, &g.mm) != 2 || g.hh < 0 || g.hh > 23 || g.mm < 0 || g.mm > 59 ||
        *end || minutes < 1 || minutes > 24 * 60) {
        return false;
    }
    if (strcmp(f[4], "-") != 0) {
        long khz = strtol(f[4], &end, 10);
        if (*end || khz < 100 || khz > 450000) return false;
        g.khz = (int)khz;
    }
    snprintf(g.net, sizeof(g.net), "%s", f[0]);
    g.wday    = wday;
    g.minutes = (int)minutes;
    snprintf(g.mode, sizeof(g.mode), "%s", f[5]);
    snprintf(g.what, sizeof(g.what), "%s", *rest ? rest : f[5]);
    s->segs[s->nsegs++] = g;
    return true;
}

static ghostnet_sched_t sched;
static char             sched_path[160];
static time_t           sched_mtime = -1;
static ino_t            sched_ino;

const ghostnet_sched_t *ghostnet_sched(void) {
    struct stat st;
    const char *path = GHOSTNET_NETS_USER;
    FILE       *f;
    char        line[200];

    if (stat(path, &st) != 0) {
        path = GHOSTNET_NETS;
        if (stat(path, &st) != 0) {
            memset(&sched, 0, sizeof(sched));
            sched_mtime = 0;
            return &sched;
        }
    }
    if (st.st_mtime == sched_mtime && st.st_ino == sched_ino && !strcmp(path, sched_path)) return &sched;
    sched_mtime = st.st_mtime;
    sched_ino   = st.st_ino;
    snprintf(sched_path, sizeof(sched_path), "%s", path);
    memset(&sched, 0, sizeof(sched));
    f = fopen(path, "r");
    if (!f) return &sched;
    while (fgets(line, sizeof(line), f)) ghostnet_sched_parse_line(&sched, line);
    fclose(f);
    return &sched;
}

const char *ghostnet_net_name(const ghostnet_sched_t *s, const char *id) {
    for (int i = 0; i < s->nnets; i++) {
        if (!strcmp(s->nets[i].id, id)) return s->nets[i].name;
    }
    return id;
}

time_t ghostnet_next_utc(time_t now, int wday, int hh, int mm, int minutes) {
    struct tm t;
    gmtime_r(&now, &t);
    time_t day0  = now - (t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec);
    int    ahead = (wday - t.tm_wday + 7) % 7;
    time_t s     = day0 + (time_t)ahead * 86400 + hh * 3600 + mm * 60;

    if (s + minutes * 60 <= now) s += 7 * 86400;                /* over for this week */
    if (s - 7 * 86400 + minutes * 60 > now) s -= 7 * 86400;     /* last week's still goes on (one across midnight) */
    return s;
}

time_t ghostnet_next_local(time_t now, int wday, int hh, int mm, int minutes) {
    struct tm t;
    localtime_r(&now, &t);
    int ahead = (wday - t.tm_wday + 7) % 7;

    for (int week = -1; week <= 1; week++) {
        struct tm x = t;
        x.tm_mday += ahead + week * 7;
        x.tm_hour  = hh;
        x.tm_min   = mm;
        x.tm_sec   = 0;
        x.tm_isdst = -1;        /* the zone's summer time on that day, not today's */
        time_t s   = mktime(&x);
        if (s + minutes * 60 > now) return s;
    }
    return now + 7 * 86400;
}

static int slot_cmp(const void *a, const void *b) {
    time_t x = ((const ghostnet_slot_t *)a)->start, y = ((const ghostnet_slot_t *)b)->start;
    return x < y ? -1 : x > y;
}

int ghostnet_upcoming_from(const ghostnet_sched_t *s, const ghostnet_conf_t *c, time_t now, ghostnet_slot_t *out,
                           int max, int hours, bool only_reminded) {
    ghostnet_slot_t all[GHOSTNET_SEGS_MAX + 1];
    int             n = 0;

    for (int i = 0; i < s->nsegs; i++) {
        const ghostnet_seg_t *g = &s->segs[i];
        if (only_reminded && !ghostnet_net_reminded(c, g->net)) continue;
        time_t start = ghostnet_next_utc(now, g->wday, g->hh, g->mm, g->minutes);
        if (start > now + (time_t)hours * 3600) continue;
        ghostnet_slot_t *o = &all[n++];
        o->start   = start;
        o->minutes = g->minutes;
        o->khz     = g->khz;
        snprintf(o->net, sizeof(o->net), "%s", g->net);
        snprintf(o->name, sizeof(o->name), "%s", ghostnet_net_name(s, g->net));
        snprintf(o->what, sizeof(o->what), "%s", g->what);
        snprintf(o->mode, sizeof(o->mode), "%s", g->mode);
    }
    if (c->own_on && (!only_reminded || ghostnet_net_reminded(c, GHOSTNET_OWN))) {
        time_t start = ghostnet_next_local(now, c->own_day, c->own_hh, c->own_mm, c->own_min);
        if (start <= now + (time_t)hours * 3600) {
            ghostnet_slot_t *o = &all[n++];
            o->start   = start;
            o->minutes = c->own_min;
            o->khz     = c->own_khz;
            snprintf(o->net, sizeof(o->net), "%s", GHOSTNET_OWN);
            snprintf(o->name, sizeof(o->name), "%s", c->own_name);
            snprintf(o->what, sizeof(o->what), "%s", c->own_mode);
            snprintf(o->mode, sizeof(o->mode), "%s", c->own_mode);
        }
    }
    qsort(all, n, sizeof(all[0]), slot_cmp);
    if (n > max) n = max;
    memcpy(out, all, n * sizeof(all[0]));
    return n;
}

int ghostnet_upcoming(time_t now, ghostnet_slot_t *out, int max, int hours, bool only_reminded) {
    return ghostnet_upcoming_from(ghostnet_sched(), ghostnet_conf(), now, out, max, hours, only_reminded);
}

void ghostnet_when(time_t t, char *out, size_t size) {
    /* the day in the screen's language (the schedule file's own days stay English: ghostnet_load reads them) */
    struct tm u, l;
    char      zone[16];

    gmtime_r(&t, &u);
    localtime_r(&t, &l);
    strftime(zone, sizeof(zone), "%Z", &l);
    if (l.tm_gmtoff == 0) {     /* the radio on UTC (or GMT): the one time */
        snprintf(out, size, "%s %02d:%02d UTC", _(day_names[u.tm_wday]), u.tm_hour, u.tm_min);
    } else if (l.tm_wday != u.tm_wday) {
        snprintf(out, size, "%s %02d:%02d UTC (%s %02d:%02d %s)", _(day_names[u.tm_wday]), u.tm_hour, u.tm_min,
                 _(day_names[l.tm_wday]), l.tm_hour, l.tm_min, zone);
    } else {
        snprintf(out, size, "%s %02d:%02d UTC (%02d:%02d %s)", _(day_names[u.tm_wday]), u.tm_hour, u.tm_min, l.tm_hour,
                 l.tm_min, zone);
    }
}

int ghostnet_remind_pick(ghostnet_remind_t *r, const ghostnet_slot_t *slots, int n, time_t now, int remind_min) {
    for (int i = 0; i < n; i++) {
        const ghostnet_slot_t *s = &slots[i];
        long                   before = (long)(s->start - now);
        bool                   said = false;
        char                   key[24];

        if (before <= 0 || before > remind_min * 60L) continue;
        /* At a net's start: its JS8 window (the RTTY and voice windows follow within the hour); the own net as it is */
        if (strcmp(s->net, GHOSTNET_OWN) != 0 && strcmp(s->mode, "JS8") != 0) continue;
        /* A net's first JS8 window only: a JS8 window that starts as another of the same net ends is not a new net */
        bool follows = false;
        for (int k = 0; k < n; k++) {
            if (k != i && !strcmp(slots[k].net, s->net) && slots[k].start + slots[k].minutes * 60 == s->start)
                follows = true;
        }
        if (follows) continue;
        snprintf(key, sizeof(key), "%s %lld", s->net, (long long)s->start);
        for (int k = 0; k < GHOSTNET_SAID; k++) {
            if (!strcmp(r->said[k], key)) said = true;
        }
        if (said) continue;
        snprintf(r->said[r->next], sizeof(r->said[0]), "%s", key);
        r->next = (r->next + 1) % GHOSTNET_SAID;
        return i;
    }
    return -1;
}

/* ---- The groups ---- */

static const struct {
    char a2[3], a3[4];
} iso3[] = {
#include "ghostnet_iso3.inc"
};

bool ghostnet_iso3(const char *iso2, char out[4]) {
    if (!iso2 || strlen(iso2) != 2) return false;
    for (size_t i = 0; i < sizeof(iso3) / sizeof(iso3[0]); i++) {
        if (!strncasecmp(iso3[i].a2, iso2, 2)) {
            memcpy(out, iso3[i].a3, 4);
            return true;
        }
    }
    return false;
}

void ghostnet_region_group(const char *iso2, const char *dept, const char *manual, char *out, size_t size) {
    char a3[4], d[4], m[16];

    alnum_upper(m, sizeof(m), manual ? manual : "");
    if (m[0]) {
        snprintf(out, size, "@%s", m);
        return;
    }
    if (!ghostnet_iso3(iso2, a3)) {
        if (size) out[0] = 0;
        return;
    }
    alnum_upper(d, sizeof(d), dept ? dept : "");
    snprintf(out, size, "@GN%s%s", a3, d);
}

bool ghostnet_groups_have(const char *groups, const char *group) {
    size_t      n = strlen(group);
    const char *p = groups;

    if (!n) return false;
    while ((p = strcasestr(p, group)) != NULL) {
        if ((p == groups || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) return true;
        p += n;
    }
    return false;
}

void ghostnet_groups_add(char *groups, size_t size, const char *region) {
    const char *add[3] = {"@GHOSTNET", "@GSTFLASH", region ? region : ""};

    for (int i = 0; i < 3; i++) {
        size_t len = strlen(groups);
        if (!add[i][0] || ghostnet_groups_have(groups, add[i])) continue;
        if (len + strlen(add[i]) + 2 > size) break;
        snprintf(groups + len, size - len, "%s%s", len ? " " : "", add[i]);
    }
}

bool ghostnet_is_group(const char *to) {
    return to && (!strcasecmp(to, "@GHOSTNET") || !strcasecmp(to, "@GSTFLASH") ||
                  (!strncasecmp(to, "@GN", 3) && to[3]));
}

bool ghostnet_is_flash(const char *to, const char *text) {
    static const char word[] = "@GSTFLASH";
    const size_t      n = sizeof(word) - 1;

    if (to && !strcasecmp(to, word)) return true;
    for (const char *p = text; p && (p = strcasestr(p, word)) != NULL; p += n) {
        bool starts = p == text || !isalnum((unsigned char)p[-1]);
        bool ends   = !isalnum((unsigned char)p[n]) && p[n] != '/';
        if (starts && ends) return true;
    }
    return false;
}

void ghostnet_box_text(const char *text, char *out, size_t size) {
    size_t k = 0;

    if (!size) return;
    for (; *text && k + 1 < size; text++) {
        if (*text == '#') {
            if (k + 2 >= size) break;   /* a lone '#' at the end would start a colour */
            out[k++] = '#';
        }
        out[k++] = *text;
    }
    out[k] = 0;
}

/* ---- The messages' log ---- */

void ghostnet_log(char kind, uint32_t id, const char *from, const char *to, int snr, int32_t dial_hz, const char *text) {
    struct stat st;
    char        clean[1024];
    FILE       *f;

    if (stat(GHOSTNET_LOG, &st) == 0 && st.st_size > GHOSTNET_LOG_MAX) rename(GHOSTNET_LOG, GHOSTNET_LOG ".1");
    f = fopen(GHOSTNET_LOG, "a");
    if (!f) return;
    snprintf(clean, sizeof(clean), "%s", text ? text : "");
    for (char *p = clean; *p; p++) {
        if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
    }
    fprintf(f, "%c\t%lld\t%u\t%s\t%s\t%d\t%d\t%s\n", kind, (long long)time(NULL), (unsigned)id, from ? from : "",
            to ? to : "", snr, (int)dial_hz, clean);
    fclose(f);
}

/* ---- The translation helper's words ---- */

void ghostnet_tr_lang(const char *gui_code, char *out, size_t size) {
    size_t n = 0;

    if (!gui_code || !gui_code[0] || !strcmp(gui_code, "en") || strlen(gui_code) > 10) {
        snprintf(out, size, "en");
        return;
    }
    /* "zh_CN" as MyMemory writes it, "zh-CN"; anything but letters, digits and the one separator is not a code */
    for (const char *p = gui_code; *p && n + 1 < size; p++) {
        if (*p == '_' || *p == '-') out[n++] = '-';
        else if (isalnum((unsigned char)*p)) out[n++] = *p;
        else {
            snprintf(out, size, "en");
            return;
        }
    }
    out[n] = '\0';
}

const char *ghostnet_tr_verb(ghostnet_tr_kind_t kind) {
    switch (kind) {
        case GHOSTNET_TR_RX:  return "rx";
        case GHOSTNET_TR_TX:  return "tx";
        default:              return "ask";
    }
}

ghostnet_tr_result_t ghostnet_tr_parse(const char *line, char *text, size_t size) {
    char   word[8];
    size_t n = 0;

    text[0] = '\0';
    while (line[n] && line[n] != ' ' && line[n] != '\n' && n + 1 < sizeof(word)) {
        word[n] = line[n];
        n++;
    }
    word[n] = '\0';
    const char *rest = line + n;
    while (*rest == ' ') rest++;
    snprintf(text, size, "%s", rest);
    /* one line: the end of the line and anything after it go */
    text[strcspn(text, "\r\n")] = '\0';
    if (!strcmp(word, "ok")) return text[0] ? GHOSTNET_TR_OK : GHOSTNET_TR_BAD;
    if (!strcmp(word, "skip")) return GHOSTNET_TR_SKIP;
    if (!strcmp(word, "limit")) return GHOSTNET_TR_LIMIT;
    if (!strcmp(word, "error") && !strcmp(text, "net")) return GHOSTNET_TR_NET;
    return GHOSTNET_TR_BAD;
}
