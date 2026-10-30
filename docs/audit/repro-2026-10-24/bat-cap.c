/* Issue #49: the battery % shown on receive (src/bat_watch.c, linked whole), the base's or the voltage's, for
 * readings as bases sent them on battery: one per 0.1 V step of a discharge, the base's own % beside the voltage.
 * Prints the % shown at each: ./bat-cap */
#include "src/bat_watch.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t vbat;       /* 0.1 V */
    uint8_t batcap;     /* the base's % */
} reading_t;

static void run(const char *name, const reading_t *r, int n) {
    bat_watch_t w;
    char        b[512] = "";

    memset(&w, 0, sizeof(w));
    for (int i = 0; i < n; i++) {
        snprintf(b + strlen(b), sizeof(b) - strlen(b), "%s%d.%d V %d", i ? ", " : "", r[i].vbat / 10, r[i].vbat % 10,
                 bat_watch_cap(&w, r[i].vbat, r[i].batcap));
    }
    printf("%s: %s\n", name, b);
}

#define RUN(name, ...)                                              \
    do {                                                            \
        static const reading_t r[] = { __VA_ARGS__ };               \
        run(name, r, sizeof(r) / sizeof(r[0]));                     \
    } while (0)

int main(void) {
    /* #49's X6100 (base 1.1.9), its discharge of 27-28 October: 0-3 % all the way, a pack charged with the radio off.
     * The pack still ran 80 minutes from the first 7.3 V, 42 from 7.2 V and 15 from 7.1 V; 7.3 and 7.2 V
     * alternated at the step */
    RUN("base at 0-3 %",
        {82, 3}, {81, 2}, {80, 1}, {79, 0}, {78, 0}, {77, 0}, {76, 0}, {75, 0}, {74, 0}, {73, 0}, {72, 0}, {73, 0},
        {72, 0}, {71, 0}, {70, 0}, {69, 0}, {68, 0}, {65, 0});
    /* The same radio on 26 October: its base counted too high, 70 % at 6.8 V and 0 % two minutes before the end */
    RUN("base too high",
        {78, 91}, {77, 90}, {76, 88}, {75, 86}, {74, 80}, {73, 78}, {72, 72}, {71, 71}, {70, 70}, {68, 70}, {66, 51},
        {65, 0});
    /* A base that counts right, as on the bench X6100 (1.1.8): never more than 10 from the voltage's */
    RUN("base that counts right",
        {83, 100}, {82, 97}, {80, 90}, {78, 78}, {76, 63}, {74, 45}, {73, 26}, {72, 12}, {71, 4}, {69, 1}, {65, 0});
    /* A base stuck higher: it would have stayed at 15 % to the end, under the 10 % warning never */
    RUN("base at 15 %", {75, 15}, {74, 15}, {73, 15}, {72, 15}, {71, 15}, {69, 15});
    /* #49's radio switched on with the pack at 7.3 V, its base at 0 %: never 25 apart from here down. And a base
     * that counts right, switched on there: shown as it is, its 0 % at 6.8 V too */
    RUN("base at 0 % from 7.3 V", {73, 0}, {72, 0}, {73, 0}, {72, 0}, {71, 0}, {70, 0}, {68, 0});
    RUN("base that counts right from 7.3 V", {73, 26}, {72, 12}, {72, 5}, {71, 3}, {69, 1}, {68, 0}, {65, 0});
    /* The base back, after a charge it saw or once it agrees on the long 7.4 V step */
    RUN("base back after a charge", {81, 2}, {81, 95}, {80, 90});
    RUN("base back at 7.4 V", {75, 0}, {74, 0}, {74, 35}, {73, 22});
    return 0;
}
