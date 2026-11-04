/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the FT8 app's log (src/adif.c, issue #174): an FT8, FT4, FT2 or JS8 QSO goes into ft_log.adi as the
 *  same record the day's log has for it (src/field/flog.c, through dialog_field_day_log), the log the web
 *  configurator's Logs page lists and exports. Before, the two wrote the same QSO in two ways (TIME_ON 1802 and
 *  180259, BAND 40M and 40m, FREQ 7.0740 and 7.074000, empty fields), and LoTW given both files took it for two
 *  QSOs. The record of #174's QSO is the one the Logs page exported for it; ft_log.adi reads back (adif_read, the
 *  import's reader) with its seconds. Both files start with the same header (field_adif_header): a line naming the
 *  log, ADIF 3.1.8, PROGRAMID OpenX6X00 and the firmware's version (X6100_FW, as the GUI sets it), where
 *  ft_log.adi had PROGRAMVERSION 1.0.0 and ADIF_VER 3.14 and started with a tag (no header, to ADIF). An
 *  ft_log.adi the older firmware started goes on with the new records, and reads back whole.
 */

#include "adif.h"
#include "field/flog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

static int bad, cases;

static void check(bool ok, const char *what) {
    cases++;
    if (!ok) {
        printf("WRONG %s\n", what);
        bad++;
    }
}

/* adif_read's band from a frequency: qso_log.c's, which needs the SQLite log */
qso_log_band_t qso_log_freq_to_band(uint64_t freq_hz) {
    static const struct { uint32_t lo, hi; qso_log_band_t band; } bands[] = {
        {7000, 7300, BAND_40M}, {10100, 10150, BAND_30M}, {14000, 14350, BAND_20M}, {28000, 29700, BAND_10M},
        {50000, 54000, BAND_6M},
    };
    uint32_t khz = freq_hz / 1000;

    for (int i = 0; i < LEN(bands); i++)
        if (khz >= bands[i].lo && khz <= bands[i].hi) return bands[i].band;
    return BAND_OTHER;
}

/* 2026-11-04 18:02:59 UTC, #174's QSO */
static const time_t T174 = 1793815379;

/* The QSO as the FT8 app makes it (ft8_window.c save_qso), with qso_log_record_create()'s arithmetic */
static qso_log_record_t qso(const char *call, time_t t, qso_log_mode_t mode, int sent, int rcvd, uint64_t hz,
                            const char *grid) {
    qso_log_record_t r = {.time = t, .mode = mode, .rsts = sent, .rstr = rcvd, .freq_mhz = (float)hz / 1000000,
                          .band = qso_log_freq_to_band(hz)};

    snprintf(r.local_call, sizeof(r.local_call), "EA5JTT");
    snprintf(r.remote_call, sizeof(r.remote_call), "%s", call);
    snprintf(r.local_grid, sizeof(r.local_grid), "IM99TL");
    snprintf(r.remote_grid, sizeof(r.remote_grid), "%s", grid);
    return r;
}

/* The file's text after its header */
static char *body(const char *path) {
    static char buf[8192];
    FILE       *f = fopen(path, "r");
    size_t      n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;

    if (f) fclose(f);
    buf[n] = 0;
    char *eoh = strcasestr(buf, "<eoh>");
    return eoh ? eoh + 5 + (eoh[5] == '\r') + (eoh[5 + (eoh[5] == '\r')] == '\n') : buf;
}

/* #174's QSO: the record the Logs page exported, the one ft_log.adi now has too */
static void issue_qso(void) {
    static const char want[] = "<CALL:6>EA5KDD <QSO_DATE:8>20261104 <TIME_ON:6>180259 <BAND:3>40m <FREQ:8>7.074000 "
                               "<MODE:3>FT8 <RST_SENT:3>+02 <RST_RCVD:3>-06 <GRIDSQUARE:4>IN81 "
                               "<STATION_CALLSIGN:6>EA5JTT <OPERATOR:6>EA5JTT <MY_GRIDSQUARE:6>IM99TL <eor>\n";
    qso_log_record_t r = qso("EA5KDD", T174, MODE_FT8, 2, -6, 7074000, "IN81");
    adif_log         l;

    remove("build/ft_log.adi");
    l = adif_log_init("build/ft_log.adi");
    check(l != NULL, "ft_log.adi not made");
    adif_add_qso(l, r);
    adif_log_close(l);
    if (strcmp(body("build/ft_log.adi"), want)) {
        printf("ft_log.adi: %s", body("build/ft_log.adi"));
        check(false, "#174's QSO in ft_log.adi: not the Logs page's record");
    }
}

/* The QSOs of each app, on HF and 6 m: the same record in ft_log.adi and the day's log, without empty fields */
static void both_logs(void) {
    const qso_log_record_t qsos[] = {
        qso("EA5KDD", T174, MODE_FT8, 2, -6, 7074000, "IN81"),
        qso("DL1ABC", T174 + 75, MODE_FT4, -12, -9, 14080000, "JO62"),
        qso("K1ABC", T174 + 140, MODE_FT2, 5, 0, 28074000, ""),
        qso("ON6URE", T174 + 3601, MODE_JS8, -3, 10, 7078000, "JO21"),
        qso("G4ABC/P", T174 + 7220, MODE_FT8, -20, -15, 50313000, "IO91"),
        qso("JA1ABC", T174 + 86400, MODE_FT8, -1, -24, 10136000, "PM95"),
    };
    field_station_t sta = {.call = "EA5JTT", .grid = "IM99TL"};
    adif_log        l;

    remove("build/ft_log.adi");
    remove("build/day.adi");
    l = adif_log_init("build/ft_log.adi");
    for (int i = 0; i < LEN(qsos); i++) {
        field_qso_t q = adif_field_qso(&qsos[i]);

        adif_add_qso(l, qsos[i]);
        check(field_log_add("build/day.adi", &q, &sta), "day's log not written");   /* as day_log_add() does */
    }
    adif_log_close(l);

    char ft[8192];
    snprintf(ft, sizeof(ft), "%s", body("build/ft_log.adi"));
    const char *day = body("build/day.adi");
    if (strcmp(ft, day)) {
        printf("ft_log.adi:\n%s\nday's log:\n%s", ft, day);
        check(false, "ft_log.adi and the day's log: not the same records");
    }
    check(!strstr(ft, ":0>"), "ft_log.adi: an empty field");
    check(!strstr(ft, "\r"), "ft_log.adi: a CR in the records");
    check(strstr(ft, "<TIME_ON:6>180414 <BAND:3>20m <FREQ:9>14.080000 <MODE:4>MFSK <SUBMODE:3>FT4 <RST_SENT:3>-12 ") != NULL,
          "FT4's QSO: not MFSK / FT4 at 14.080000 and 18:04:14");
    check(strstr(ft, "<FREQ:9>28.074000 <MODE:4>MFSK <SUBMODE:3>FT2 <RST_SENT:3>+05 <RST_RCVD:3>+00 <STATION") != NULL,
          "FT2's QSO: not MFSK / FT2 at 28.074000, or a grid it had none of");
    check(strstr(ft, "<CALL:7>G4ABC/P <QSO_DATE:8>20261104 <TIME_ON:6>200319 <BAND:2>6m <FREQ:9>50.313000 ") != NULL,
          "6 m QSO: not 6m at 50.313000");
    check(strstr(ft, "<QSO_DATE:8>20261105 <TIME_ON:6>180259 <BAND:3>30m <FREQ:9>10.136000 ") != NULL,
          "next day's QSO: not 20261105 on 30m");

    /* Read back as the import reads a file: the seconds and the rest as they went in */
    qso_log_record_t *back = NULL;
    int               skipped = -1;
    int               n = adif_read("build/ft_log.adi", &back, &skipped);
    char              what[96];

    check(n == LEN(qsos) && skipped == 0, "ft_log.adi read back: not all its QSOs");
    for (int i = 0; i < n && i < LEN(qsos); i++) {
        snprintf(what, sizeof(what), "QSO %d read back: not as written", i);
        check(back[i].time == qsos[i].time && !strcmp(back[i].remote_call, qsos[i].remote_call) &&
              back[i].mode == qsos[i].mode && back[i].rsts == qsos[i].rsts && back[i].rstr == qsos[i].rstr &&
              back[i].band == qsos[i].band && back[i].freq_mhz == qsos[i].freq_mhz &&
              !strcmp(back[i].local_call, "EA5JTT") && !strcmp(back[i].remote_grid, qsos[i].remote_grid), what);
    }
    free(back);
}

/* The two logs' headers, with the firmware's version and without (a PC test with no X6100_FW) */
static void headers(void) {
    static const char tail[] = "\n<ADIF_VER:5>3.1.8 <PROGRAMID:9>OpenX6X00 <PROGRAMVERSION:5>0.3.8 <eoh>\n";
    char              want[160], got[160];
    FILE             *f;
    size_t            n;

    snprintf(want, sizeof(want), "X6100 FT8, FT4 and FT2 log%s<CALL:", tail);
    f = fopen("build/ft_log.adi", "r");
    n = f ? fread(got, 1, strlen(want), f) : 0;
    got[n] = 0;
    if (f) fclose(f);
    check(!strcmp(got, want), "ft_log.adi's header: not as the day's log's");
    snprintf(want, sizeof(want), "X6100 Field Ops log%s<CALL:", tail);
    f = fopen("build/day.adi", "r");
    n = f ? fread(got, 1, strlen(want), f) : 0;
    got[n] = 0;
    if (f) fclose(f);
    check(!strcmp(got, want), "the day's log's header: not the same");

    unsetenv("X6100_FW");
    f = fopen("build/header.adi", "w+");
    field_adif_header(f, "X6100 FT8, FT4 and FT2 log");
    rewind(f);
    n = fread(got, 1, sizeof(got) - 1, f);
    got[n] = 0;
    fclose(f);
    check(!strcmp(got, "X6100 FT8, FT4 and FT2 log\n<ADIF_VER:5>3.1.8 <PROGRAMID:9>OpenX6X00 <eoh>\n"),
          "header without X6100_FW: not without PROGRAMVERSION");
    setenv("X6100_FW", "0.3.8", 1);
}

/* An ft_log.adi the older firmware started (its header, #174's QSO as it wrote it) goes on with the new records */
static void older_file(void) {
    static const char old[] = "<PROGRAMID:9>OpenX6X00\r\n<PROGRAMVERSION:5>1.0.0\r\n<ADIF_VER:4>3.14\r\n<EOH>\r\n"
                              "<STATION_CALLSIGN:6>EA5JTT<OPERATOR:6>EA5JTT<CALL:6>EA5KDD<QSO_DATE:8>20261104"
                              "<QSO_DATE_OFF:8>20261104<TIME_ON:4>1802<TIME_OFF:4>1802<MODE:3>FT8<SUBMODE:0>"
                              "<NAME:0><QTH:0><RST_SENT:3>+02<STX:0><RST_RCVD:3>-06<BAND:3>40M<FREQ:6>7.0740"
                              "<GRIDSQUARE:4>IN81<MY_GRIDSQUARE:6>IM99TL<DISTANCE:3>316<EOR>\r\n";
    FILE             *f = fopen("build/ft_log.adi", "w");
    adif_log          l;

    fputs(old, f);
    fclose(f);
    l = adif_log_init("build/ft_log.adi");
    adif_add_qso(l, qso("DL1ABC", T174 + 75, MODE_FT4, -12, -9, 14080000, "JO62"));
    adif_log_close(l);

    qso_log_record_t *back = NULL;
    int               n = adif_read("build/ft_log.adi", &back, NULL);

    check(n == 2, "older ft_log.adi with a new QSO: not 2 QSOs read back");
    check(n == 2 && back[0].time == T174 - 59 && back[0].band == BAND_40M && back[0].mode == MODE_FT8,
          "older ft_log.adi: its QSO not read back at 18:02 on 40M");
    check(n == 2 && back[1].time == T174 + 75 && back[1].mode == MODE_FT4 && !strcmp(back[1].remote_call, "DL1ABC"),
          "older ft_log.adi: the new QSO not read back");
    check(strstr(body("build/ft_log.adi"), "<EOR>\r\n<CALL:6>DL1ABC <QSO_DATE:8>20261104 <TIME_ON:6>180414 ") != NULL,
          "older ft_log.adi: the new record not after the old one");
    free(back);
}

int main(void) {
    setenv("X6100_FW", "0.3.8", 1);     /* gui_main.c's */
    issue_qso();
    both_logs();
    headers();
    older_file();
    printf("%d cases, %d wrong\n", cases, bad);
    return bad != 0;
}
