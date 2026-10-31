/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of Field Ops' references (src/field/flog.c; issue #81): the programme a reference is taken for by its
 *  shape (field_ref_prog: the other station's reference typed after the call in Log QSO), the tag the log shows for
 *  it, and the ADIF fields an activation and that reference go in as (field_adif_record). An LLOTA lake is LL, the
 *  country's two letters and four or five digits: 1,181 of llota.app's lakes (LLAR, LLTR, LLFR, LLBR, LLPR, LLGR,
 *  LLCR) end their prefix in R as the towers do, the others have a park's shape. The web configurator's Logs page
 *  tells them apart the same way (webserver apps.py ref_prog; docs/audit/repro-2026-10-24/log_checks.py).
 */

#include "field/flog.h"

#include <stdio.h>
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

/* A reference of each programme, the lakes with the prefixes the towers' rule would take */
static void programmes(void) {
    static const struct {
        const char *ref;
        char        want;
    } want[] = {
        {"LLPL-0001", 'K'},  {"LLPL-0914", 'K'}, {"LLAR-0393", 'K'}, {"LLTR-0258", 'K'},  {"LLFR-0227", 'K'},
        {"LLBR-0139", 'K'},  {"LLPR-0110", 'K'}, {"LLGR-0052", 'K'}, {"LLCR-0002", 'K'},  {"LLIE-10001", 'K'},
        {"LLBE-0026", 'K'},  {"LLNZ-1049", 'K'}, {"OKR-0150", 'T'},  {"DLR-0163", 'T'},   {"ONR-0323", 'T'},
        {"9MR-0101", 'T'},   {"K1R-0029", 'T'},  {"US-1234", 'P'},   {"KR-1308", 'P'},    {"FR-10234", 'P'},
        {"K-12345", 'P'},    {"SP-0001", 'P'},   {"DL-01156", 'C'},  {"I-14619", 'C'},    {"3D2-00001", 'C'},
        {"X42018", 'M'},     {"BEL 002", 'L'},   {"BEL-002", 'L'},   {"USA 1286", 'L'},   {"DLFF-0123", 'F'},
        {"SPFF-0001", 'F'},  {"RO-H0390", 'H'},  {"I/LO-123", 'S'},  {"SP/BZ-001", 'S'},  {"B/G-0070", 'B'},
        {"JO20ab", 'G'},
    };
    char what[64];

    for (int i = 0; i < LEN(want); i++) {
        char got = field_ref_prog(want[i].ref);

        snprintf(what, sizeof(what), "%s: '%c', not '%c'", want[i].ref, got ? got : '0', want[i].want);
        check(got == want[i].want, what);
    }
}

/* The log's tag for their reference: lake to lake is LL2LL (L2L is the lighthouses') */
static void tags(void) {
    static const struct {
        const char *their, *want;
    } want[] = {
        {"LLPL-0002", "LL2LL"}, {"LLAR-0393", "LL2LL"}, {"OKR-0150", "T2T"}, {"US-1234", "P2P"}, {"BEL 002", "L2L"},
    };
    char what[64];

    for (int i = 0; i < LEN(want); i++) {
        field_qso_t q = {0};

        snprintf(q.their_ref, sizeof(q.their_ref), "%s", want[i].their);
        snprintf(what, sizeof(what), "tag of %s: %s, not %s", want[i].their, field_qso_tag(&q), want[i].want);
        check(!strcmp(field_qso_tag(&q), want[i].want), what);
    }
}

/* An LLOTA activation's record: MY_SIG LLOTA / MY_SIG_INFO, which llota.app reads from the ADIF upload; their lake as
 * SIG LLOTA / SIG_INFO, never a park's POTA_REF or a tower's SIG TOTA */
static void records(void) {
    static const char *const mine = "<MY_SIG:5>LLOTA <MY_SIG_INFO:9>LLPL-0076 ";
    static const struct {
        const char *their;
        const char *has, *not;  /* in the fields after the activation's own */
    } want[] = {
        {"LLAR-0393", "<SIG:5>LLOTA <SIG_INFO:9>LLAR-0393 ", "TOTA"},
        {"LLPL-0002", "<SIG:5>LLOTA <SIG_INFO:9>LLPL-0002 ", "POTA"},
        {"LLIE-10001", "<SIG:5>LLOTA <SIG_INFO:10>LLIE-10001 ", "POTA"},
        {"US-1234", "<SIG:4>POTA <SIG_INFO:7>US-1234 <POTA_REF:7>US-1234 ", "MY_POTA_REF"},
        {"", "<eor>", "<SIG:"},
    };
    field_station_t sta = {.call = "SQ9SZ", .grid = "JO90", .prog = 'K', .ref = "LLPL-0076"};
    char            rec[768], what[96];

    for (int i = 0; i < LEN(want); i++) {
        field_qso_t q = {.t = 1793000000, .call = "SP9ABC", .freq_hz = 7154000, .mode = "SSB", .rst_s = "59",
                         .rst_r = "57"};
        const char *who = want[i].their[0] ? want[i].their : "no reference";

        snprintf(q.their_ref, sizeof(q.their_ref), "%s", want[i].their);
        field_adif_record(rec, sizeof(rec), &q, &sta);
        const char *theirs = strstr(rec, mine);
        snprintf(what, sizeof(what), "record with %s: no %s", who, mine);
        check(theirs != NULL, what);
        theirs = theirs ? theirs + strlen(mine) : rec;
        snprintf(what, sizeof(what), "record with %s: no %s", who, want[i].has);
        check(strstr(theirs, want[i].has) != NULL, what);
        snprintf(what, sizeof(what), "record with %s: has %s", who, want[i].not);
        check(strstr(theirs, want[i].not) == NULL, what);
    }
}

int main(void) {
    programmes();
    tags();
    records();
    printf("%d cases, %d wrong\n", cases, bad);
    return bad != 0;
}
