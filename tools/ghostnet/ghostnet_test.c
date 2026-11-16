/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of GhostNet in the JS8 app (src/ghostnet.c): the regional group from a country and a department, the
 *  groups added, the @GSTFLASH test and the message box's text of a station's words, the schedule file the image
 *  ships, the next window of a net and the reminder's choice across midnight (UTC and local) and a week's end, the
 *  times shown in UTC with the local zone, the settings file, the translation helper's language code and its answers,
 *  the messages' log.
 *  translate_test.py checks the helper itself (rootfs/usr/sbin/x6100-translate) without the network.
 */
#define _GNU_SOURCE
#include "ghostnet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails, checks;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        checks++;                                                                   \
        if (!(cond)) {                                                              \
            fails++;                                                                \
            fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                           \
    } while (0)

#define CHECK_STR(got, want)                                                        \
    do {                                                                            \
        checks++;                                                                   \
        if (strcmp((got), (want)) != 0) {                                           \
            fails++;                                                                \
            fprintf(stderr, "%s:%d: FAILED: \"%s\", wanted \"%s\"\n", __FILE__, __LINE__, (got), (want)); \
        }                                                                           \
    } while (0)

/* No language here: the English, as the GUI with none chosen */
const char *i18n_tr(const char *s) {
    return s;
}

/* The test's clock: a UTC date and time */
static time_t utc(int y, int mo, int d, int h, int mi) {
    struct tm t = {.tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi};
    return timegm(&t);
}

static void zone(const char *tz) {
    setenv("TZ", tz, 1);
    tzset();
}

/* The schedule the image ships */
static ghostnet_sched_t shipped;

static void load_shipped(void) {
    FILE *f = fopen(GHOSTNET_NETS, "r");
    char  line[200];

    CHECK(f != NULL);
    if (!f) return;
    while (fgets(line, sizeof(line), f)) ghostnet_sched_parse_line(&shipped, line);
    fclose(f);
}

static void test_groups(void) {
    char out[32], a3[4];

    CHECK(ghostnet_iso3("UY", a3) && !strcmp(a3, "URY"));
    CHECK(ghostnet_iso3("be", a3) && !strcmp(a3, "BEL"));
    CHECK(!ghostnet_iso3("XX", a3));
    CHECK(!ghostnet_iso3("", a3));
    CHECK(!ghostnet_iso3("BEL", a3));

    ghostnet_region_group("UY", "MO", "", out, sizeof(out));
    CHECK_STR(out, "@GNURYMO");
    ghostnet_region_group("be", "vov", NULL, out, sizeof(out));
    CHECK_STR(out, "@GNBELVOV");
    ghostnet_region_group("UY", "Mo-1x", "", out, sizeof(out));   /* letters and digits, three at most */
    CHECK_STR(out, "@GNURYMO1");
    ghostnet_region_group("BE", "", "", out, sizeof(out));        /* the country alone */
    CHECK_STR(out, "@GNBEL");
    ghostnet_region_group("", "MO", "", out, sizeof(out));        /* no country (none set yet): no group */
    CHECK_STR(out, "");
    ghostnet_region_group("ZZ", "MO", "", out, sizeof(out));
    CHECK_STR(out, "");
    ghostnet_region_group("", "", "@gn ury-mo", out, sizeof(out)); /* typed by hand: as typed, cleaned */
    CHECK_STR(out, "@GNURYMO");
    ghostnet_region_group("BE", "VOV", "GNURYMO", out, sizeof(out)); /* the typed one wins */
    CHECK_STR(out, "@GNURYMO");

    char groups[96] = "";
    ghostnet_groups_add(groups, sizeof(groups), "@GNURYMO");
    CHECK_STR(groups, "@GHOSTNET @GSTFLASH @GNURYMO");
    snprintf(groups, sizeof(groups), "@X6100 @ghostnet");
    ghostnet_groups_add(groups, sizeof(groups), "");
    CHECK_STR(groups, "@X6100 @ghostnet @GSTFLASH");      /* @ghostnet is @GHOSTNET: not twice */
    char small[16] = "@X6100";
    ghostnet_groups_add(small, sizeof(small), "@GNURYMO");
    CHECK(strlen(small) < sizeof(small));
    CHECK_STR(small, "@X6100");                           /* "@X6100 @GHOSTNET" + NUL is 17: nothing fits */

    CHECK(ghostnet_groups_have("@X6100 @GSTFLASH", "@gstflash"));
    CHECK(!ghostnet_groups_have("@GHOSTNETX", "@GHOSTNET"));
    CHECK(!ghostnet_groups_have("@X@GHOSTNET", "@GHOSTNET"));
    CHECK(!ghostnet_groups_have("@GHOSTNET", ""));

    CHECK(ghostnet_is_group("@GHOSTNET"));
    CHECK(ghostnet_is_group("@gstflash"));
    CHECK(ghostnet_is_group("@GNURYMO"));
    CHECK(!ghostnet_is_group("@GN"));
    CHECK(!ghostnet_is_group("CX1ABC"));
    CHECK(!ghostnet_is_group("@HB"));
    CHECK(!ghostnet_is_group(NULL));
}

static void test_flash(void) {
    char out[64];

    CHECK(ghostnet_is_flash("@GSTFLASH", "CX1ABC: @GSTFLASH FIRE"));
    CHECK(ghostnet_is_flash("@gstflash", ""));
    CHECK(ghostnet_is_flash("@GHOSTNET", "CX1ABC: @GHOSTNET PASS TO @GSTFLASH: ROAD CLOSED"));
    CHECK(ghostnet_is_flash("", "(@GSTFLASH) FLOOD"));
    CHECK(!ghostnet_is_flash("@GHOSTNET", "CX1ABC: @GHOSTNET ALL OK"));
    CHECK(!ghostnet_is_flash("", "CX1ABC: @GSTFLASHES"));     /* another group */
    CHECK(!ghostnet_is_flash("", "CX1ABC: X@GSTFLASH"));
    CHECK(!ghostnet_is_flash("", "CX1ABC: @GSTFLASH/P"));
    CHECK(!ghostnet_is_flash(NULL, NULL));

    /* The box colours "#rrggbb text#": a station's '#' doubled shows as one and colours nothing */
    ghostnet_box_text("A#B", out, sizeof(out));
    CHECK_STR(out, "A##B");
    ghostnet_box_text("#ff0000 FAKE# OK", out, sizeof(out));
    CHECK_STR(out, "##ff0000 FAKE## OK");
    ghostnet_box_text("A#B", out, 4);
    CHECK_STR(out, "A##");
    ghostnet_box_text("A#B", out, 3);                         /* no lone '#' at the cut */
    CHECK_STR(out, "A");
    ghostnet_box_text("PLAIN", out, sizeof(out));
    CHECK_STR(out, "PLAIN");
}

static void test_schedule_file(void) {
    ghostnet_sched_t s = {0};
    char             line[120];

    CHECK(shipped.nnets == 7);
    CHECK(shipped.nsegs == 23);
    CHECK_STR(ghostnet_net_name(&shipped, "EU"), "Europe");
    CHECK_STR(ghostnet_net_name(&shipped, "B_NAEU"), "Bridge North America-Europe");
    CHECK_STR(ghostnet_net_name(&shipped, "XX"), "XX");
    /* the first window: NA, Friday 01:00 UTC, 30 min, 7.107 JS8 */
    CHECK_STR(shipped.segs[0].net, "NA");
    CHECK(shipped.segs[0].wday == 5 && shipped.segs[0].hh == 1 && shipped.segs[0].mm == 0);
    CHECK(shipped.segs[0].minutes == 30 && shipped.segs[0].khz == 7107);
    CHECK_STR(shipped.segs[0].mode, "JS8");
    CHECK_STR(shipped.segs[0].what, "JS8 40 m");
    CHECK_STR(shipped.segs[2].what, "RTTY 45.45 baud");
    for (int i = 0; i < shipped.nsegs; i++) {
        if (!strcmp(shipped.segs[i].mode, "ALE")) CHECK(shipped.segs[i].khz == 0);   /* "-": none */
    }

    const char *bad[] = {"XX Fri 25:00 30 7107 JS8 x", "XX Fri 01:00 30 abc JS8 x", "XX Fry 01:00 30 7107 JS8 x",
                         "XX Fri 01:00 0 7107 JS8 x", "XX Fri 01:00 30 7107", "net", "   ", "# a comment"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        snprintf(line, sizeof(line), "%s", bad[i]);
        CHECK(!ghostnet_sched_parse_line(&s, line));
    }
    CHECK(s.nnets == 0 && s.nsegs == 0);
    snprintf(line, sizeof(line), "net  ES   Red hispana   # the own Spanish net\n");
    CHECK(ghostnet_sched_parse_line(&s, line));
    CHECK_STR(s.nets[0].id, "ES");
    CHECK_STR(s.nets[0].name, "Red hispana");
    snprintf(line, sizeof(line), "ES thu 21:00 30 7107 JS8\n");   /* no description: the mode */
    CHECK(ghostnet_sched_parse_line(&s, line));
    CHECK(s.segs[0].wday == 4);
    CHECK_STR(s.segs[0].what, "JS8");
}

static void test_next(void) {
    time_t thu17 = utc(2026, 11, 19, 17, 0);

    zone("UTC0");
    /* Europe's net, Thursday 18:00 UTC, 30 min */
    CHECK(ghostnet_next_utc(thu17, 4, 18, 0, 30) == utc(2026, 11, 19, 18, 0));
    CHECK(ghostnet_next_utc(utc(2026, 11, 19, 18, 10), 4, 18, 0, 30) == utc(2026, 11, 19, 18, 0));  /* going on */
    CHECK(ghostnet_next_utc(utc(2026, 11, 19, 18, 30), 4, 18, 0, 30) == utc(2026, 11, 26, 18, 0));  /* over */
    CHECK(ghostnet_next_utc(utc(2026, 11, 20, 9, 0), 4, 18, 0, 30) == utc(2026, 11, 26, 18, 0));
    /* the week's end: Saturday 23:58 to Sunday 00:05 */
    CHECK(ghostnet_next_utc(utc(2026, 11, 21, 23, 58), 0, 0, 5, 30) == utc(2026, 11, 22, 0, 5));
    /* a window across midnight, Saturday 23:30 for an hour, still going on at Sunday 00:10 */
    CHECK(ghostnet_next_utc(utc(2026, 11, 22, 0, 10), 6, 23, 30, 60) == utc(2026, 11, 21, 23, 30));
    CHECK(ghostnet_next_utc(utc(2026, 11, 22, 0, 30), 6, 23, 30, 60) == utc(2026, 11, 28, 23, 30));

    /* the own net in local time: 21:00 on Thursday in Uruguay (UTC-3) is 00:00 UTC on Friday */
    zone("<-03>3");
    CHECK(ghostnet_next_local(utc(2026, 11, 19, 23, 0), 4, 21, 0, 30) == utc(2026, 11, 20, 0, 0));
    CHECK(ghostnet_next_local(utc(2026, 11, 20, 0, 20), 4, 21, 0, 30) == utc(2026, 11, 20, 0, 0)); /* going on */
    CHECK(ghostnet_next_local(utc(2026, 11, 20, 0, 30), 4, 21, 0, 30) == utc(2026, 11, 27, 0, 0));
    /* Central Europe: 21:00 is 19:00 UTC in summer, 20:00 in winter */
    zone("CET-1CEST,M3.5.0,M10.5.0/3");
    CHECK(ghostnet_next_local(utc(2026, 7, 16, 12, 0), 4, 21, 0, 30) == utc(2026, 7, 16, 19, 0));
    CHECK(ghostnet_next_local(utc(2026, 11, 19, 12, 0), 4, 21, 0, 30) == utc(2026, 11, 19, 20, 0));
    zone("UTC0");
}

static void test_when(void) {
    char out[64];

    zone("UTC0");
    ghostnet_when(utc(2026, 11, 19, 18, 0), out, sizeof(out));
    CHECK_STR(out, "Thu 18:00 UTC");
    zone("CET-1CEST,M3.5.0,M10.5.0/3");
    ghostnet_when(utc(2026, 11, 19, 18, 0), out, sizeof(out));
    CHECK_STR(out, "Thu 18:00 UTC (19:00 CET)");
    ghostnet_when(utc(2026, 7, 16, 18, 0), out, sizeof(out));
    CHECK_STR(out, "Thu 18:00 UTC (20:00 CEST)");
    /* North America's net: Friday 01:00 UTC is Thursday evening there */
    zone("EST5EDT,M3.2.0,M11.1.0");
    ghostnet_when(utc(2026, 11, 20, 1, 0), out, sizeof(out));
    CHECK_STR(out, "Fri 01:00 UTC (Thu 20:00 EST)");
    zone("UTC0");
}

static void test_upcoming_and_remind(void) {
    ghostnet_conf_t   c;
    ghostnet_slot_t   s[16];
    ghostnet_remind_t r = {0};
    int               n;

    zone("UTC0");
    ghostnet_conf_defaults(&c);
    c.remind = true;
    snprintf(c.nets, sizeof(c.nets), "EU");

    /* Europe's evening: its four windows, by start */
    n = ghostnet_upcoming_from(&shipped, &c, utc(2026, 11, 19, 17, 55), s, 16, 3, true);
    CHECK(n == 4);
    CHECK(n == 4 && !strcmp(s[0].mode, "JS8") && s[0].start == utc(2026, 11, 19, 18, 0));
    CHECK(n == 4 && !strcmp(s[1].mode, "VARA") && !strcmp(s[2].mode, "RTTY") && !strcmp(s[3].mode, "LSB"));
    CHECK(n == 4 && !strcmp(s[0].name, "Europe") && s[0].khz == 7107);
    /* every net, not only those reminded: the next 7 days have every window once */
    CHECK(ghostnet_upcoming_from(&shipped, &c, utc(2026, 11, 19, 17, 55), s, 16, 7 * 24, false) == 16);
    ghostnet_slot_t all[GHOSTNET_SEGS_MAX];
    CHECK(ghostnet_upcoming_from(&shipped, &c, utc(2026, 11, 19, 17, 55), all, GHOSTNET_SEGS_MAX, 7 * 24, false) ==
          23);

    /* 10 minutes before: not at 17:49:59, at 17:50, once */
    time_t at = utc(2026, 11, 19, 17, 49) + 59;
    n = ghostnet_upcoming_from(&shipped, &c, at, s, 16, 3, true);
    CHECK(ghostnet_remind_pick(&r, s, n, at, 10) == -1);
    at = utc(2026, 11, 19, 17, 50);
    n = ghostnet_upcoming_from(&shipped, &c, at, s, 16, 3, true);
    CHECK(ghostnet_remind_pick(&r, s, n, at, 10) == 0);
    at = utc(2026, 11, 19, 17, 51);
    n = ghostnet_upcoming_from(&shipped, &c, at, s, 16, 3, true);
    CHECK(ghostnet_remind_pick(&r, s, n, at, 10) == -1);
    /* held back by a transmission until 17:58: it comes then; at the start it is too late */
    memset(&r, 0, sizeof(r));
    at = utc(2026, 11, 19, 17, 58);
    n = ghostnet_upcoming_from(&shipped, &c, at, s, 16, 3, true);
    CHECK(ghostnet_remind_pick(&r, s, n, at, 10) == 0);
    memset(&r, 0, sizeof(r));
    at = utc(2026, 11, 19, 18, 0);
    n = ghostnet_upcoming_from(&shipped, &c, at, s, 16, 3, true);
    CHECK(ghostnet_remind_pick(&r, s, n, at, 10) == -1);
    /* the RTTY window is not a reminder of its own */
    memset(&r, 0, sizeof(r));
    at = utc(2026, 11, 19, 18, 55);
    n = ghostnet_upcoming_from(&shipped, &c, at, s, 16, 3, true);
    CHECK(ghostnet_remind_pick(&r, s, n, at, 10) == -1);

    /* North America's net across midnight UTC: Friday 01:00, 70 minutes before is Thursday 23:50 */
    snprintf(c.nets, sizeof(c.nets), "NA");
    memset(&r, 0, sizeof(r));
    at = utc(2026, 11, 19, 23, 55);
    n = ghostnet_upcoming_from(&shipped, &c, at, s, 16, 3, true);
    int i = ghostnet_remind_pick(&r, s, n, at, 70);
    CHECK(i >= 0 && !strcmp(s[i].net, "NA") && s[i].start == utc(2026, 11, 20, 1, 0));

    /* A bridge: its first JS8 window, not the one after its ALE window */
    snprintf(c.nets, sizeof(c.nets), "B_NAAU");
    memset(&r, 0, sizeof(r));
    at = utc(2026, 11, 21, 13, 25);
    n = ghostnet_upcoming_from(&shipped, &c, at, s, 16, 3, true);
    CHECK(ghostnet_remind_pick(&r, s, n, at, 10) == -1);
    at = utc(2026, 11, 21, 11, 52);
    n = ghostnet_upcoming_from(&shipped, &c, at, s, 16, 3, true);
    i = ghostnet_remind_pick(&r, s, n, at, 10);
    CHECK(i >= 0 && s[i].start == utc(2026, 11, 21, 12, 0) && s[i].khz == 14107);

    /* Two nets due at once each come once, in turn */
    snprintf(c.nets, sizeof(c.nets), "EU,OWN");
    c.own_on  = true;
    c.own_day = 4;
    c.own_hh  = 18;
    c.own_mm  = 0;
    memset(&r, 0, sizeof(r));
    at = utc(2026, 11, 19, 17, 55);
    n = ghostnet_upcoming_from(&shipped, &c, at, s, 16, 3, true);
    int first = ghostnet_remind_pick(&r, s, n, at, 10), second = ghostnet_remind_pick(&r, s, n, at, 10);
    CHECK(first >= 0 && second >= 0 && first != second);
    CHECK(ghostnet_remind_pick(&r, s, n, at, 10) == -1);
    CHECK(ghostnet_remind_pick(&r, s, n, at + 20, 10) == -1);

    /* The own net in local time across midnight UTC: Thursday 21:00 in Uruguay, 10 minutes before is 23:50 UTC */
    zone("<-03>3");
    snprintf(c.nets, sizeof(c.nets), "OWN");
    c.own_hh = 21;
    snprintf(c.own_name, sizeof(c.own_name), "Red hispana");
    memset(&r, 0, sizeof(r));
    at = utc(2026, 11, 19, 23, 52);
    n = ghostnet_upcoming_from(&shipped, &c, at, s, 16, 3, true);
    i = ghostnet_remind_pick(&r, s, n, at, 10);
    CHECK(i >= 0 && !strcmp(s[i].net, GHOSTNET_OWN) && !strcmp(s[i].name, "Red hispana") &&
          s[i].start == utc(2026, 11, 20, 0, 0));
    /* off: not in the lists */
    c.own_on = false;
    CHECK(ghostnet_upcoming_from(&shipped, &c, at, s, 16, 3, true) == 0);
    zone("UTC0");
}

static void test_conf(void) {
    ghostnet_conf_t c, d;
    char            line[128];

    ghostnet_conf_defaults(&c);
    CHECK(!c.remind && c.remind_min == 10 && !c.nets[0] && !c.own_on);
    CHECK(!c.translate_rx && !c.translate_tx);  /* nothing goes to MyMemory until the operator says so */
    const char *lines[] = {"remind=1", "remind_min=500", "nets=EU,OWN", "own_on=1", "own_name=Red hispana",
                           "own_day=9", "own_time=20:45", "own_min=3", "own_khz=7107", "own_mode=FT8",
                           "dept=v-o-v2", "region=@GNBELVOV", "# remind=0", "nonsense", "translate_rx=1"};
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        snprintf(line, sizeof(line), "%s\n", lines[i]);
        ghostnet_conf_parse_line(&c, line);
    }
    CHECK(c.remind);
    CHECK(c.remind_min == 10);          /* 500 is out of range */
    CHECK_STR(c.nets, "EU,OWN");
    CHECK(c.own_on && !strcmp(c.own_name, "Red hispana"));
    CHECK(c.own_day == 4);              /* 9 is no day */
    CHECK(c.own_hh == 20 && c.own_mm == 45);
    CHECK(c.own_min == 30);             /* 3 is too short */
    CHECK_STR(c.own_mode, "JS8");       /* FT8 is not one of them */
    CHECK_STR(c.dept, "VOV");
    CHECK_STR(c.region, "@GNBELVOV");
    CHECK(c.translate_rx && !c.translate_tx);

    /* toggling a net */
    ghostnet_conf_defaults(&d);
    ghostnet_net_toggle(&d, "EU");
    CHECK_STR(d.nets, "EU");
    ghostnet_net_toggle(&d, "NA");
    ghostnet_net_toggle(&d, "EU");
    CHECK_STR(d.nets, "NA");
    ghostnet_net_toggle(&d, "B_NAEU");
    CHECK_STR(d.nets, "NA,B_NAEU");
    CHECK(ghostnet_net_reminded(&d, "B_NAEU") && !ghostnet_net_reminded(&d, "B_NA") && !ghostnet_net_reminded(&d, "EU"));
    CHECK_STR(ghostnet_region_net(1), "EU");
    CHECK_STR(ghostnet_region_net(2), "NA");
    CHECK_STR(ghostnet_region_net(3), "AU");

    /* written and read back */
    CHECK(ghostnet_conf_save(&c));
    const ghostnet_conf_t *got = ghostnet_conf();
    CHECK(got->remind && !strcmp(got->nets, "EU,OWN") && got->own_hh == 20 && got->own_mm == 45);
    CHECK(!strcmp(got->own_name, "Red hispana") && !strcmp(got->dept, "VOV") && !strcmp(got->region, "@GNBELVOV"));
    CHECK(got->translate_rx && !got->translate_tx);
}

/* The messages' log the web page reads: one line a message, tabs and line ends of a station's text as spaces */
static void test_log(void) {
    char  line[512];
    FILE *f;
    int   n = 0;

    ghostnet_log('R', 7, "LU1AAA", "@GHOSTNET", -5, 7107000, "BUENAS\tNOCHES\nA TODOS");
    ghostnet_log('T', 7, "LU1AAA", "@GHOSTNET", -5, 7107000, "Good evening everyone");
    ghostnet_log('X', 0, NULL, NULL, 0, 7107000, NULL);
    f = fopen(GHOSTNET_LOG, "r");
    CHECK(f != NULL);
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char kind, from[16] = "", text[128] = "";
        long long when;
        unsigned  id;
        int       snr, dial;
        n++;
        if (n == 1) {
            CHECK(sscanf(line, "%c\t%lld\t%u\t%15[^\t]\t%*[^\t]\t%d\t%d\t%127[^\n]", &kind, &when, &id, from, &snr,
                         &dial, text) == 7);
            CHECK(kind == 'R' && id == 7 && snr == -5 && dial == 7107000 && when > 1700000000LL);
            CHECK_STR(from, "LU1AAA");
            CHECK_STR(text, "BUENAS NOCHES A TODOS");
        }
        if (n == 3) CHECK(!strncmp(line, "X\t", 2) && strstr(line, "\t0\t\t\t0\t7107000\t\n") != NULL);
    }
    fclose(f);
    CHECK(n == 3);
}

static void test_translate_words(void) {
    char out[64], text[GHOSTNET_TR_TEXT + 1];

    /* the GUI's language as MyMemory writes it */
    ghostnet_tr_lang("", out, sizeof(out));
    CHECK_STR(out, "en");
    ghostnet_tr_lang(NULL, out, sizeof(out));
    CHECK_STR(out, "en");
    ghostnet_tr_lang("es", out, sizeof(out));
    CHECK_STR(out, "es");
    ghostnet_tr_lang("nl", out, sizeof(out));
    CHECK_STR(out, "nl");
    ghostnet_tr_lang("zh_CN", out, sizeof(out));
    CHECK_STR(out, "zh-CN");
    ghostnet_tr_lang("x;rm -rf", out, sizeof(out));    /* a translator's file of a strange name: English */
    CHECK_STR(out, "en");
    ghostnet_tr_lang("abcdefghijklmnop", out, sizeof(out));
    CHECK_STR(out, "en");

    CHECK_STR(ghostnet_tr_verb(GHOSTNET_TR_RX), "rx");
    CHECK_STR(ghostnet_tr_verb(GHOSTNET_TR_ASK), "ask");
    CHECK_STR(ghostnet_tr_verb(GHOSTNET_TR_TX), "tx");

    /* the helper's one line */
    CHECK(ghostnet_tr_parse("ok Buenas noches a todos\n", text, sizeof(text)) == GHOSTNET_TR_OK);
    CHECK_STR(text, "Buenas noches a todos");
    CHECK(ghostnet_tr_parse("ok\n", text, sizeof(text)) == GHOSTNET_TR_BAD);       /* no text: no translation */
    CHECK(ghostnet_tr_parse("skip\n", text, sizeof(text)) == GHOSTNET_TR_SKIP);
    CHECK(ghostnet_tr_parse("limit 9\n", text, sizeof(text)) == GHOSTNET_TR_LIMIT);
    CHECK_STR(text, "9");
    CHECK(ghostnet_tr_parse("error net\n", text, sizeof(text)) == GHOSTNET_TR_NET);
    CHECK(ghostnet_tr_parse("error answer\n", text, sizeof(text)) == GHOSTNET_TR_BAD);
    CHECK(ghostnet_tr_parse("", text, sizeof(text)) == GHOSTNET_TR_BAD);             /* the helper did not run */
    CHECK(ghostnet_tr_parse("Traceback (most recent call last):", text, sizeof(text)) == GHOSTNET_TR_BAD);
    CHECK(ghostnet_tr_parse("ok one line\nok another\n", text, sizeof(text)) == GHOSTNET_TR_OK);
    CHECK_STR(text, "one line");
    char small[8];
    CHECK(ghostnet_tr_parse("ok a long translation", small, sizeof(small)) == GHOSTNET_TR_OK);
    CHECK_STR(small, "a long ");
}

int main(void) {
    load_shipped();
    test_groups();
    test_flash();
    test_schedule_file();
    test_next();
    test_when();
    test_upcoming_and_remind();
    test_conf();
    test_translate_words();
    test_log();
    printf("ghostnet_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
