/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of CapsLock+Space, the keyboard's PTT toggle (src/kbd_ptt.c; #175). The chord, the outcome of its key,
 *  the ticks with what the radio says (the GUI's PTT on, the radio on the air, the base's data) and the keyboard going
 *  away, as screen_main.c gives them; what comes out is what it then does. The toggle never leaves the radio keyed by
 *  surprise: off after a refused key, it follows a TX ended another way (the next chord keys again), and lets go of
 *  a key that never got on the air, of a radio back on receive with the PTT on, when the base's data stops, after
 *  three minutes, and when the keyboard goes.
 */

#include "kbd_ptt.h"

#include <stdio.h>

static int bad, cases;

static void check(bool ok, const char *what) {
    cases++;
    if (!ok) {
        printf("WRONG %s\n", what);
        bad++;
    }
}

/* The chord, keyed with this outcome when it says to key. True when it said to key */
static bool chord(kbd_ptt_t *p, bool ptt, bool ok, uint64_t now) {
    bool key = kbd_ptt_chord(p, ptt);

    if (key) kbd_ptt_keyed(p, ok, now);
    return key;
}

int main(void) {
    kbd_ptt_t p = {0};

    /* On and off */
    check(chord(&p, false, true, 0), "first chord: key");
    check(p.on, "keyed: on");
    check(kbd_ptt_tick(&p, true, false, false, 100) == KBD_PTT_STAY, "keyed, not on the air yet after 100 ms: stays");
    check(kbd_ptt_tick(&p, true, true, false, 200) == KBD_PTT_STAY, "on the air: stays");
    check(kbd_ptt_tick(&p, true, true, false, KBD_PTT_LIMIT_MS) == KBD_PTT_STAY, "on the air three minutes on: stays");
    check(!chord(&p, true, true, KBD_PTT_LIMIT_MS), "second chord: let go");
    check(!p.on, "let go: off");
    check(kbd_ptt_tick(&p, false, false, false, KBD_PTT_LIMIT_MS + 100) == KBD_PTT_STAY, "off: a tick does nothing");
    check(chord(&p, false, true, KBD_PTT_LIMIT_MS + 200), "third chord: key again");

    /* A refused key leaves it off, and the next chord tries again */
    p = (kbd_ptt_t){0};
    check(chord(&p, false, false, 0), "chord where the radio may not transmit: asks to key");
    check(!p.on, "refused: off");
    check(kbd_ptt_tick(&p, false, false, false, 100) == KBD_PTT_STAY, "refused: no tick does anything");
    check(chord(&p, false, true, 200), "the chord after a refusal keys, not lets go");
    check(p.on, "keyed after the refusal: on");

    /* TX ended another way (the PTT key let go, SWR, a band stop, CAT): off, the next chord keys */
    p = (kbd_ptt_t){0};
    chord(&p, false, true, 0);
    kbd_ptt_tick(&p, true, true, false, 100);
    check(kbd_ptt_tick(&p, false, false, false, 200) == KBD_PTT_ENDED, "the PTT went off another way: ended");
    check(!p.on, "ended: off");
    check(kbd_ptt_tick(&p, false, false, false, 300) == KBD_PTT_STAY, "ended: told once");
    check(chord(&p, false, true, 400), "the chord after: keys again, not 'lets go'");

    /* The same before a tick saw it: the chord looks at the PTT itself */
    p = (kbd_ptt_t){0};
    chord(&p, false, true, 0);
    kbd_ptt_tick(&p, true, true, false, 100);
    check(chord(&p, false, true, 150), "PTT gone, no tick since: the chord keys");
    check(p.on && !p.on_air, "keyed anew: on, not seen on the air yet");

    /* The PTT off while the base still says TX (the base's hang): ended all the same */
    p = (kbd_ptt_t){0};
    chord(&p, false, true, 0);
    check(kbd_ptt_tick(&p, false, true, false, 100) == KBD_PTT_ENDED, "PTT off, the base still in TX: ended");

    /* A key that never got on the air: let go once KBD_PTT_TAKE_MS is over */
    p = (kbd_ptt_t){0};
    chord(&p, false, true, 1000);
    check(kbd_ptt_tick(&p, true, false, false, 1000 + KBD_PTT_TAKE_MS) == KBD_PTT_STAY,
          "not on the air at the limit: stays");
    check(kbd_ptt_tick(&p, true, false, false, 1001 + KBD_PTT_TAKE_MS) == KBD_PTT_LET_GO, "past the limit: let go");
    check(!p.on, "let go: off");
    check(chord(&p, false, true, 5000), "the chord after: keys");

    /* Slow to key, in time: stays */
    p = (kbd_ptt_t){0};
    chord(&p, false, true, 0);
    kbd_ptt_tick(&p, true, false, false, KBD_PTT_TAKE_MS - 100);
    check(kbd_ptt_tick(&p, true, true, false, KBD_PTT_TAKE_MS - 50) == KBD_PTT_STAY, "on the air just in time: stays");
    check(kbd_ptt_tick(&p, true, true, false, 10 * KBD_PTT_TAKE_MS) == KBD_PTT_STAY, "and stays on the air");

    /* Back on receive with the PTT on (the base dropped TX): let go at once, it would key again later */
    p = (kbd_ptt_t){0};
    chord(&p, false, true, 0);
    kbd_ptt_tick(&p, true, true, false, 100);
    check(kbd_ptt_tick(&p, true, false, false, 200) == KBD_PTT_LET_GO,
          "on the air, then receive with the PTT on: let go");
    check(!p.on, "let go: off");

    /* No data from the base while on: let go at once (no SWR protection, no power reading, radio.c keeps the TX) */
    p = (kbd_ptt_t){0};
    chord(&p, false, true, 0);
    kbd_ptt_tick(&p, true, true, false, 100);
    check(kbd_ptt_tick(&p, true, true, true, 3200) == KBD_PTT_NO_DATA, "on the air, the base's data stops: let go");
    check(!p.on, "no data: off");
    check(chord(&p, false, true, 4000), "the chord after: keys (radio_set_ptt refuses it while there is none)");
    p = (kbd_ptt_t){0};
    chord(&p, false, true, 0);
    check(kbd_ptt_tick(&p, true, false, true, 100) == KBD_PTT_NO_DATA,
          "keyed, no data before it got on the air: let go");
    check(kbd_ptt_tick(&p, false, false, true, 200) == KBD_PTT_STAY, "off: no data does nothing");
    p = (kbd_ptt_t){0};
    chord(&p, false, true, 0);
    check(kbd_ptt_tick(&p, false, true, true, 100) == KBD_PTT_ENDED,
          "PTT off another way, and no data: ended, nothing to let go");

    /* Three minutes keyed: let go, as the browser's PTT and TCI's */
    p = (kbd_ptt_t){0};
    chord(&p, false, true, 1000);
    kbd_ptt_tick(&p, true, true, false, 1100);
    check(kbd_ptt_tick(&p, true, true, false, 1000 + KBD_PTT_LIMIT_MS) == KBD_PTT_STAY,
          "on the air at the limit: stays");
    check(kbd_ptt_tick(&p, true, true, false, 1001 + KBD_PTT_LIMIT_MS) == KBD_PTT_TIME_UP, "past the limit: let go");
    check(!p.on, "time up: off");
    check(chord(&p, false, true, 2000 + KBD_PTT_LIMIT_MS), "the chord after: keys again");
    check(kbd_ptt_tick(&p, true, true, false, 2100 + KBD_PTT_LIMIT_MS) == KBD_PTT_STAY,
          "keyed anew: the time starts again");
    check(kbd_ptt_tick(&p, true, true, false, 2001 + 2 * KBD_PTT_LIMIT_MS) == KBD_PTT_TIME_UP,
          "and ends three minutes on");

    /* The PTT already keyed by someone else (the browser): the chord keys (taken over), the next one lets go */
    p = (kbd_ptt_t){0};
    check(chord(&p, true, true, 0), "off, the PTT on from elsewhere: the chord keys");
    check(!chord(&p, true, true, 100), "the next chord lets go");

    /* The keyboard going away */
    p = (kbd_ptt_t){0};
    chord(&p, false, true, 0);
    kbd_ptt_tick(&p, true, true, false, 100);
    check(kbd_ptt_gone(&p, true), "keyboard gone while on: let go");
    check(!p.on, "gone: off");
    check(!kbd_ptt_gone(&p, true), "gone again: nothing (the PTT on from elsewhere stays)");
    p = (kbd_ptt_t){0};
    check(!kbd_ptt_gone(&p, false), "keyboard gone while off: nothing");
    chord(&p, false, true, 0);
    check(!kbd_ptt_gone(&p, false), "keyboard gone, the PTT already off: nothing to let go");
    check(!p.on, "and off");
    p = (kbd_ptt_t){0};
    chord(&p, false, false, 0);
    check(!kbd_ptt_gone(&p, true), "keyboard gone after a refused key: nothing (not ours)");

    printf("%d cases, %d wrong\n", cases, bad);
    return bad != 0;
}
