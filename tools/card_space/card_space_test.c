/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the room left on the card (src/card_space.c, #341). The rule of the start's warning: Off never
 *  warns; each choice warns under its megabytes and not at them; a full card warns, an empty one does not; a
 *  partition smaller than four times the choice warns under a quarter of itself, so a small one that is merely
 *  small stays quiet; a size that is not known never warns; a value that is no choice is Off. The default is 100 MB.
 *  The sizes as shown: megabytes under 1000 MB, a GB with one decimal under 10 GB, whole GB above, always rounded
 *  down. The read of a real filesystem (this folder's) and of a path that is not there.
 */

#include "card_space.h"

#include <stdio.h>
#include <string.h>

#define MB (1024ull * 1024ull)
#define GB (1024ull * MB)

static int bad, cases;

static void check(bool ok, const char *what) {
    cases++;
    if (!ok) {
        printf("WRONG %s\n", what);
        bad++;
    }
}

static void text_is(uint64_t bytes, const char *want) {
    char s[24], what[96];

    card_space_text(bytes, s, sizeof(s));
    snprintf(what, sizeof(what), "%llu bytes shown as \"%s\", wanted \"%s\"", (unsigned long long)bytes, s, want);
    check(!strcmp(s, want), what);
}

int main(void) {
    static const struct {
        int      choice;
        uint32_t mb;
    } choices[] = {{CARD_WARN_100MB, 100}, {CARD_WARN_250MB, 250}, {CARD_WARN_500MB, 500}, {CARD_WARN_1GB, 1024}};
    uint64_t room, size;
    char     s[64];

    /* The choices */
    check(card_warn_mb(CARD_WARN_OFF) == 0, "Off has megabytes");
    check(card_warn_mb(-1) == 0 && card_warn_mb(CARD_WARN_LAST + 1) == 0, "a value that is no choice has megabytes");
    check(CARD_WARN_DEFAULT == CARD_WARN_100MB, "the default is not 100 MB");
    for (size_t i = 0; i < sizeof(choices) / sizeof(choices[0]); i++) {
        int      c = choices[i].choice;
        uint64_t under = choices[i].mb * MB;

        check(card_warn_mb(c) == choices[i].mb, "a choice's megabytes");
        /* a 32 GB card: the choice itself */
        check(card_warn_under(32 * GB, c) == under, "a big card: not the choice's megabytes");
        check(card_space_low(under - 1, 32 * GB, c), "a byte under the choice does not warn");
        check(!card_space_low(under, 32 * GB, c), "exactly the choice warns");
        check(!card_space_low(under + MB, 32 * GB, c), "above the choice warns");
        check(card_space_low(0, 32 * GB, c), "a full card does not warn");
        check(!card_space_low(32 * GB, 32 * GB, c), "an empty card warns");
        /* the size not known: never */
        check(!card_space_low(0, 0, c), "a size not known warns");
    }
    /* Off, whatever is free; and a value that is no choice */
    check(!card_space_low(0, 32 * GB, CARD_WARN_OFF), "Off warns");
    check(card_warn_under(32 * GB, CARD_WARN_OFF) == 0, "Off has a level");
    check(!card_space_low(0, 32 * GB, 99) && !card_space_low(0, 32 * GB, -3), "a value that is no choice warns");

    /* A small partition: a quarter of it in place of the choice */
    check(card_warn_under(200 * MB, CARD_WARN_100MB) == 50 * MB, "200 MB partition, 100 MB: not 50 MB");
    check(!card_space_low(60 * MB, 200 * MB, CARD_WARN_100MB), "200 MB partition with 60 MB free warns at 100 MB");
    check(card_space_low(49 * MB, 200 * MB, CARD_WARN_100MB), "200 MB partition with 49 MB free does not warn");
    check(card_warn_under(400 * MB, CARD_WARN_100MB) == 100 * MB, "400 MB partition, 100 MB: not the choice itself");
    check(card_warn_under(2 * GB, CARD_WARN_1GB) == 512 * MB, "2 GB partition, 1 GB: not 512 MB");
    check(!card_space_low(600 * MB, 2 * GB, CARD_WARN_1GB), "2 GB partition with 600 MB free warns at 1 GB");
    check(card_space_low(600 * MB, 8 * GB, CARD_WARN_1GB), "8 GB partition with 600 MB free does not warn at 1 GB");
    /* a new, empty small partition never warns at any choice */
    for (int c = CARD_WARN_OFF; c <= CARD_WARN_LAST; c++) {
        check(!card_space_low(64 * MB, 64 * MB, c), "an empty 64 MB partition warns");
        check(!card_space_low(900 * MB, 1 * GB, c), "a 1 GB partition with 900 MB free warns");
    }

    /* The sizes as shown */
    text_is(0, "0 MB");
    text_is(MB - 1, "0 MB");
    text_is(85 * MB + 500000, "85 MB");
    text_is(999 * MB, "999 MB");
    text_is(1000 * MB, "0.9 GB");
    text_is(GB, "1.0 GB");
    text_is(GB + 300 * MB, "1.2 GB");
    text_is(10 * GB - 1, "9.9 GB");
    text_is(10 * GB, "10 GB");
    text_is(29 * GB + 900 * MB, "29 GB");
    text_is(238 * GB, "238 GB");
    card_space_line(GB + 300 * MB, 29 * GB + 100 * MB, s, sizeof(s));
    check(!strcmp(s, "1.2 GB free of 29 GB"), "the line of a card");
    card_space_line(85 * MB, 3 * GB + 700 * MB, s, sizeof(s));
    check(!strcmp(s, "85 MB free of 3.6 GB"), "the line of a small card");

    /* The read */
    room = size = 0;
    check(card_space_read(".", &room, &size) && size > 0 && room <= size, "this folder's filesystem is not read");
    check(!card_space_read("/no/such/folder/at/all", &room, &size), "a path that is not there is read");

    printf("%d cases, %d wrong\n", cases, bad);
    return bad ? 1 : 0;
}
