/* Issue #111: the user's frequency lock and an app's hold. run.py cuts the lock's code out of the production
 * src/screen_main.c into freq_lock_cut.c (from the end of cw_mode() to the end of toggle_freq_lock(), and from
 * main_screen_lock_freq() to the end of main_screen_freq_locked()), with the screen's state before it and two
 * calls after it for this file: lock_key() is the LOCK key (SPCH, Scroll Lock and the Frequency lock action call
 * the same toggle_freq_lock()), lock_init() gives it its subject. The LOCK key, the browser Panel's LOCK and the
 * apps (FT8, JS8, PSK... hold the frequency while open, main_screen_lock_freq()) run in turn, and each case says
 * whether the frequency is locked (what the knob, F-INP, TCI, the Panel's frequency, Pat's QSY and Field Ops check),
 * whether the user's lock is on (the Panel's "ulock", its LOCK button) and whether LOCK shows in the strip.
 * Ends "N of N cases as they should be". */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct SubjectInt {
    int32_t v;
} SubjectInt;

void main_screen_lock_freq(bool lock);
bool main_screen_freq_locked(void);
void lock_key(void);
void lock_init(SubjectInt *s);

static SubjectInt subject;
static bool       tag;      /* LOCK in the frequency strip */

int32_t subject_i_get(SubjectInt *s) { return s->v; }
void subject_i_set(SubjectInt *s, int32_t v) { s->v = v; }
void freq_strip_set_lock(bool user_lock) { tag = user_lock; }
void info_lock_mode(bool lock) {}

/* Before #111 there was one switch: the Panel's LOCK set it as the apps did, and its button showed it */
__attribute__((weak)) void main_screen_user_lock_freq(bool lock) { main_screen_lock_freq(lock); }
__attribute__((weak)) bool main_screen_freq_user_locked(void) { return main_screen_freq_locked(); }

static int cases, good;

static void check(const char *what, bool locked, bool user, bool shown) {
    char got[64], want[64];

    snprintf(got, sizeof(got), "locked %d, user's lock %d, LOCK shown %d", main_screen_freq_locked(),
             main_screen_freq_user_locked(), tag);
    snprintf(want, sizeof(want), "locked %d, user's lock %d, LOCK shown %d", locked, user, shown);
    cases++;
    good += !strcmp(got, want);
    printf("%s: %s%s\n", what, got, strcmp(got, want) ? "   <-- not as it should be" : "");
}

int main(void) {
    lock_init(&subject);
    check("at the start", false, false, false);

    /* The user's lock through an app: still on after it closes */
    lock_key();
    check("LOCK pressed", true, true, true);
    main_screen_lock_freq(true);
    check("FT8 opened", true, true, true);
    main_screen_lock_freq(false);
    check("FT8 closed, the lock set before it", true, true, true);
    lock_key();
    check("LOCK pressed again", false, false, false);

    /* An app's hold alone: the user's lock stays off, and ends with the app */
    main_screen_lock_freq(true);
    check("FT8 opened, no lock", true, false, false);
    main_screen_lock_freq(false);
    check("FT8 closed", false, false, false);

    /* The Panel's LOCK during an app: the user's lock, the app's hold stays */
    main_screen_lock_freq(true);
    main_screen_user_lock_freq(true);
    check("the Panel's LOCK on, FT8 open", true, true, true);
    main_screen_user_lock_freq(false);
    check("the Panel's LOCK off, FT8 open", true, false, false);
    main_screen_lock_freq(false);
    check("FT8 closed", false, false, false);

    /* LOCK pressed while an app holds the frequency */
    main_screen_lock_freq(true);
    lock_key();
    check("JS8 opened, LOCK pressed", true, true, true);
    main_screen_lock_freq(false);
    check("JS8 closed", true, true, true);
    main_screen_lock_freq(true);
    lock_key();
    check("JS8 opened again, LOCK pressed", true, false, false);
    main_screen_lock_freq(false);
    check("JS8 closed", false, false, false);

    /* The Panel's LOCK with no app, then two apps one after the other */
    main_screen_user_lock_freq(true);
    main_screen_lock_freq(true);
    main_screen_lock_freq(false);
    main_screen_lock_freq(true);
    main_screen_lock_freq(false);
    check("the Panel's LOCK on, PSK opened and closed, then Olivia", true, true, true);
    main_screen_user_lock_freq(false);
    check("the Panel's LOCK off", false, false, false);

    printf("%d of %d cases as they should be\n", good, cases);
    return good == cases ? 0 : 1;
}
