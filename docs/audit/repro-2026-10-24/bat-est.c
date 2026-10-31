/* Issue #108: the time to full on external power, with the production src/bat_est.c linked whole. Rows of
 * battery_log.csv as radio.c writes them, one a minute, each replayed through bat_est_replay() (as the radio replays
 * its log at the start) from a file in $AUDIT_TMP, and after each what the top right shows on the supply: the
 * lightning sign while the base's charging flag is on or bat_est_minutes() gives a time to full, with that time,
 * else the plug (clock.c). Prints a line per stretch: ./bat-est */
#include "src/bat_est.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T0      1792831740L     /* 24 October 2026 08:49 UTC */

typedef struct {
    int cap;        /* the base's % */
    int charging;   /* its charging flag */
} row_t;

static void run(const char *name, const row_t *r, int n) {
    const char *dir = getenv("AUDIT_TMP");
    char        path[512], b[512] = "";

    snprintf(path, sizeof(path), "%s/battery_log.csv", dir ? dir : ".");
    bat_est_reset();
    for (int i = 0; i < n; i++) {
        FILE *f = fopen(path, "w");

        if (!f) {
            perror(path);
            exit(1);
        }
        if (!i) fputs("time,vbat,batcap,vext,charging,tx,backlight,cpu_mhz,tx_s,tx_w,wifi,bt,hp\n", f);
        fprintf(f, "%ld,%.1f,%d,13.8,%d,0,2,1008,0.0,0.0,1,1,0\n", T0 + 60L * i, 7.4f + r[i].cap / 100.0f,
                r[i].cap, r[i].charging);
        fclose(f);
        bat_est_replay(path);

        bool chg = false, approx = false;
        int  m = bat_est_minutes(0, &chg, &approx);
        int  l = strlen(b);

        if (m >= 0) {
            snprintf(b + l, sizeof(b) - l, "%s%d %% bolt %s%dh%02d", i ? ", " : "", r[i].cap, approx ? "~" : "",
                     m / 60, m % 60);
        } else {
            snprintf(b + l, sizeof(b) - l, "%s%d %% %s", i ? ", " : "", r[i].cap, r[i].charging ? "bolt" : "plug");
        }
    }
    printf("%s: %s\n", name, b);
}

#define RUN(name, ...)                                              \
    do {                                                            \
        static const row_t r[] = { __VA_ARGS__ };                   \
        run(name, r, sizeof(r) / sizeof(r[0]));                     \
    } while (0)

int main(void) {
    /* kj7prs's report: an X6100 with Charger Off plugged in, the base raising no flag. Its first row gave a time to
     * full and the lightning sign for a minute, then the plug */
    RUN("Charger Off at 60 %", {60, 0}, {60, 0}, {60, 0}, {60, 0}, {60, 0});
    RUN("Charger Off at 92 %", {92, 0}, {92, 0}, {92, 0}, {92, 0}, {92, 0});
    /* Charging, the flag on from the first row */
    RUN("charging from 60 %", {60, 1}, {60, 1}, {61, 1}, {61, 1}, {62, 1});
    /* The flag one row late, as the bench X6100's on 24 October (86 %) and #49's on 30 October (0 %); at 90 % or
     * more too, where a first row without it must not end the charge */
    RUN("charging from 86 %, the flag one row late", {86, 0}, {87, 1}, {87, 1}, {87, 1}, {88, 1});
    RUN("charging from 92 %, the flag one row late", {92, 0}, {92, 1}, {93, 1}, {93, 1}, {94, 1});
    /* Later in a charge: one row without the flag below 90 % keeps the time to full; the charger stopping near the
     * top shows the plug at once */
    RUN("charging at 70 %, the flag off for a row", {70, 1}, {70, 1}, {71, 0}, {71, 1}, {71, 1});
    RUN("charged to 97 %", {96, 1}, {97, 1}, {97, 0}, {97, 0}, {97, 0}, {97, 0});
    return 0;
}
