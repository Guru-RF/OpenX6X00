/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the DX cluster's bands (src/cluster/spots.c, src/cluster/client.c; issue #103): 4 m, 2 m and 70 cm
 *  after 6 m, a spot's band and the mode it is tuned in there, and the band filter and the alerts in cluster.conf.
 *  Files of before (filter_bands=40,20,6, alerts on 20,17) read as they were; 2 m and 70 cm saved as 2 and 70, the
 *  numbers the web configurator writes too. The country list is not read: the spotters filter is not tested here.
 */

#include "cluster/client.h"
#include "cluster/spots.h"

#include <stdio.h>
#include <string.h>

#define LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

static int bad, cases;

/* client.c asks the country list for the spotters filter (filter_spotter_dxcc=): none here */
const char *dxcc_entity(const char *call) {
    (void)call;
    return NULL;
}

bool dxcc_prefix_entity(const char *prefix, const char **entity) {
    (void)prefix;
    (void)entity;
    return false;
}

static void check(bool ok, const char *what) {
    cases++;
    if (!ok) {
        printf("WRONG %s\n", what);
        bad++;
    }
}

static cluster_spot_t spot(int32_t hz, const char *dx, const char *comment) {
    cluster_spot_t s = {.freq_hz = hz, .utc_min = 12 * 60};

    snprintf(s.dx, sizeof(s.dx), "%s", dx);
    snprintf(s.spotter, sizeof(s.spotter), "DL1ABC");
    snprintf(s.comment, sizeof(s.comment), "%s", comment);
    return s;
}

static int band_index(const char *name) {
    for (int b = 0; b < 16; b++)
        if (!strcmp(cluster_band_name(b), name)) return b;
    return -1;
}

static void write_conf(const char *text) {
    FILE *f = fopen(CLUSTER_CONF, "w");

    fputs(text, f);
    fclose(f);
}

/* The line of the file that starts with `key`, "" when there is none */
static const char *conf_line(const char *key) {
    static char line[512];
    FILE       *f = fopen(CLUSTER_CONF, "r");

    while (f && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        if (!strncmp(line, key, strlen(key))) {
            fclose(f);
            return line;
        }
    }
    if (f) fclose(f);
    return "";
}

static void bands_of_frequencies(void) {
    static const struct {
        int32_t     hz;
        const char *band;
    } want[] = {
        {1840000, "160m"},    {3573000, "80m"},     {5357000, "60m"},     {7074000, "40m"},     {10136000, "30m"},
        {14074000, "20m"},    {18100000, "17m"},    {21074000, "15m"},    {24915000, "12m"},    {28074000, "10m"},
        {40680000, "8m"},     {50313000, "6m"},     {69900000, "4m"},     {70154000, "4m"},     {70450000, "4m"},
        {70500000, "4m"},     {144000000, "2m"},    {144300000, "2m"},    {145500000, "2m"},    {146520000, "2m"},
        {148000000, "2m"},    {420000000, "70cm"},  {432200000, "70cm"},  {439000000, "70cm"},  {446000000, "70cm"},
        {450000000, "70cm"},  {52500000, ""},       {55000000, ""},       {70600000, ""},       {100000000, ""},
        {143999000, ""},      {148001000, ""},      {222100000, ""},      {419999000, ""},      {450001000, ""},
        {1296200000, ""},
    };
    char what[96];
    int  n = 0;

    for (int i = 0; i < LEN(want); i++) {
        const char *got = cluster_band_name(cluster_band_of(want[i].hz));

        snprintf(what, sizeof(what), "%.3f MHz in \"%s\", not \"%s\"", want[i].hz / 1e6, got, want[i].band);
        check(!strcmp(got, want[i].band), what);
    }
    while (n < 16 && cluster_band_name(n)[0]) n++;
    check(n == 15, "15 bands: 160m to 6m, 4m, 2m, 70cm");
    check(band_index("6m") == 11 && band_index("4m") == 12 && band_index("2m") == 13 && band_index("70cm") == 14,
          "4m, 2m and 70cm after 6m, the bands before them where they were");
    check(!strcmp(cluster_band_name(15), "") && !strcmp(cluster_band_name(-1), ""), "no 16th band, no band -1");
}

static void modes_of_spots(void) {
    static const struct {
        int32_t        hz;
        const char    *comment;
        cluster_mode_t mode;
    } want[] = {
        {70050000, "", CLUSTER_MODE_CW},          {70154000, "", CLUSTER_MODE_DIGI},
        {70200000, "", CLUSTER_MODE_USB},         {70450000, "", CLUSTER_MODE_FM},
        {144050000, "", CLUSTER_MODE_CW},         {144174000, "-12 dB", CLUSTER_MODE_DIGI},
        {144300000, "JO40 tnx", CLUSTER_MODE_USB}, {144200000, "", CLUSTER_MODE_USB},
        {144800000, "", CLUSTER_MODE_FM},         {145500000, "", CLUSTER_MODE_FM},
        {146520000, "", CLUSTER_MODE_FM},         {432050000, "", CLUSTER_MODE_CW},
        {432200000, "", CLUSTER_MODE_USB},        {433500000, "", CLUSTER_MODE_FM},
        {439000000, "", CLUSTER_MODE_FM},         {446000000, "", CLUSTER_MODE_FM},
        {431000000, "", CLUSTER_MODE_FM},         {144300000, "FT8 -10", CLUSTER_MODE_DIGI},
        {145500000, "CW 599", CLUSTER_MODE_CW},   {432200000, "SSB 59", CLUSTER_MODE_USB},
        {144300000, "FM", CLUSTER_MODE_FM},       {29600000, "", CLUSTER_MODE_FM},
        {50313000, "", CLUSTER_MODE_DIGI},        {50150000, "", CLUSTER_MODE_USB},
        {14025000, "", CLUSTER_MODE_CW},          {7150000, "", CLUSTER_MODE_LSB},
    };
    static const char *names[] = {"CW", "LSB", "USB", "DIGI", "AM", "FM"};
    char what[96];

    for (int i = 0; i < LEN(want); i++) {
        cluster_spot_t s   = spot(want[i].hz, "DG1SMD", want[i].comment);
        cluster_mode_t got = cluster_guess_mode(&s);

        snprintf(what, sizeof(what), "%.3f MHz \"%s\" tuned in %s, not %s", want[i].hz / 1e6, want[i].comment,
                 names[got], names[want[i].mode]);
        check(got == want[i].mode, what);
    }
}

static void spots_from_the_cluster(void) {
    cluster_spot_t s;

    check(cluster_parse_line("DX de DL1ABC:    144300.0  DG1SMD       JO40 tnx QSO              1234Z JO62", &s) &&
              s.freq_hz == 144300000 && !strcmp(s.dx, "DG1SMD"),
          "a live 2 m spot read at 144.300 MHz");
    check(cluster_parse_line("432200.0  PA0ABC      28-Oct-2026 1238Z  JO22 tropo                  <DL1ABC>", &s) &&
              s.freq_hz == 432200000 && cluster_band_of(s.freq_hz) == band_index("70cm"),
          "a sh/dx row on 70 cm read at 432.200 MHz, in 70cm");
}

static void filter_of_before(void) {
    cluster_filter_t f = {0};
    cluster_spot_t   s20 = spot(14074000, "VP8LP", ""), s2 = spot(144300000, "DG1SMD", "");
    cluster_spot_t   s70 = spot(432200000, "PA0ABC", ""), s4 = spot(70200000, "G4ABC", "");

    /* No filter: every band, 2 m too (as before, when 2 m was no band) */
    write_conf("host=dxfun.com\nport=8000\n");
    cluster_filter_load(&f);
    check(f.bands == 0, "no filter_bands= line: no band filter");
    check(cluster_filter_pass(&f, &s20, 12 * 60) && cluster_filter_pass(&f, &s2, 12 * 60) &&
              cluster_filter_pass(&f, &s70, 12 * 60),
          "no band filter: 20 m, 2 m and 70 cm spots shown");

    /* A filter saved before: the same bands, 2 m and 70 cm still hidden */
    write_conf("host=dxfun.com\nport=8000\nfilter_bands=40,20,6\nalert=VP8*|20,17|CW\n");
    cluster_filter_load(&f);
    check(f.bands == (1u << band_index("40m") | 1u << band_index("20m") | 1u << band_index("6m")),
          "filter_bands=40,20,6 of before: 40m, 20m and 6m");
    check(cluster_filter_pass(&f, &s20, 12 * 60) && !cluster_filter_pass(&f, &s2, 12 * 60) &&
              !cluster_filter_pass(&f, &s70, 12 * 60) && !cluster_filter_pass(&f, &s4, 12 * 60),
          "filter_bands=40,20,6: 20 m shown, 4 m, 2 m and 70 cm hidden as before");
    check(f.alert_count == 1 && f.alerts[0].bands == (1u << band_index("20m") | 1u << band_index("17m")),
          "an alert of before on 20,17: 20m and 17m");

    /* 2 m and 70 cm as the web page writes them, and 70cm written by hand */
    write_conf("filter_bands=2,70\n");
    cluster_filter_load(&f);
    check(f.bands == (1u << band_index("2m") | 1u << band_index("70cm")), "filter_bands=2,70: 2m and 70cm");
    check(!cluster_filter_pass(&f, &s20, 12 * 60) && cluster_filter_pass(&f, &s2, 12 * 60) &&
              cluster_filter_pass(&f, &s70, 12 * 60) && !cluster_filter_pass(&f, &s4, 12 * 60),
          "filter_bands=2,70: 2 m and 70 cm shown, 20 m and 4 m hidden");
    write_conf("filter_bands=70cm, 4\n");
    cluster_filter_load(&f);
    check(f.bands == (1u << band_index("70cm") | 1u << band_index("4m")), "filter_bands=70cm, 4: 70cm and 4m");
    write_conf("filter_bands=23,13,222\n");
    cluster_filter_load(&f);
    check(f.bands == 0, "filter_bands=23,13,222: no band known, no filter (as before)");
}

static void filter_saved(void) {
    cluster_filter_t f = {0};
    uint16_t         vhf = 1u << band_index("6m") | 1u << band_index("4m") | 1u << band_index("2m") |
                           1u << band_index("70cm");

    write_conf("host=dxfun.com\nport=8000\nfilter_bands=40\nalert=DG1SMD|2,70|\nlogin=ON6URE-2\n");
    check(cluster_filter_save(CLUSTER_FM_ALL, vhf, 0), "saved");
    check(!strcmp(conf_line("filter_bands="), "filter_bands=6,4,2,70"), "6 m to 70 cm saved as filter_bands=6,4,2,70");
    check(!strcmp(conf_line("alert="), "alert=DG1SMD|2,70|") && !strcmp(conf_line("login="), "login=ON6URE-2") &&
              !strcmp(conf_line("host="), "host=dxfun.com"),
          "the other lines kept");
    cluster_filter_load(&f);
    check(f.bands == vhf, "read back: 6m, 4m, 2m, 70cm");

    cluster_spot_t s2 = spot(144300000, "DG1SMD", ""), s70 = spot(432200000, "DG1SMD", "");
    cluster_spot_t s20 = spot(14074000, "DG1SMD", "");
    check(f.alert_count == 1 && cluster_alert_match(&f, &s2) == 0 && cluster_alert_match(&f, &s70) == 0 &&
              cluster_alert_match(&f, &s20) < 0,
          "an alert on 2,70: DG1SMD on 144.300 and 432.200, not on 14.074");

    check(cluster_filter_save(CLUSTER_FM_ALL, 0, 0) && !conf_line("filter_bands=")[0], "all bands: no line");
}

int main(void) {
    bands_of_frequencies();
    modes_of_spots();
    spots_from_the_cluster();
    filter_of_before();
    filter_saved();
    remove(CLUSTER_CONF);
    printf("%d cases, %d wrong\n", cases, bad);
    return bad != 0;
}
