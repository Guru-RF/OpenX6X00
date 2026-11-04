/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of CapsLock as the keyboard's hyper key (src/kbd_hyper.c; issue #175). Key events go in as evdev
 *  sends them, and come out as key_input.c uses them: the keys typed (passed on to xkb and LVGL), the times Caps
 *  Lock turned, the chords' actions. CapsLock tapped alone is Caps Lock; held, a key with it is a chord, never
 *  typed, and Caps Lock stays as it was; CapsLock+L is the quick QSO log, the other letters do nothing yet. Keys
 *  pressed before CapsLock, and Shift, Ctrl, Alt and Meta, go through.
 */

#include "kbd_hyper.h"

#include <stdio.h>
#include <string.h>

static int bad, cases;

static void check(bool ok, const char *what) {
    cases++;
    if (!ok) {
        printf("WRONG %s\n", what);
        bad++;
    }
}

/* What key_input.c makes of the events */
typedef struct {
    char typed[64];     /* the events passed on: a letter per key, upper case down, lower case up, r a repeat */
    int  caps;          /* Caps Lock turned */
    int  log_qso;       /* chords with the quick QSO log */
    int  none;          /* chords with no action */
} out_t;

static char name(uint16_t code) {
    static const struct { uint16_t code; char c; } names[] = {
        {KEY_A, 'a'}, {KEY_K, 'k'}, {KEY_L, 'l'}, {KEY_X, 'x'}, {KEY_LEFTSHIFT, 's'}, {KEY_LEFTCTRL, 'c'},
        {KEY_RIGHTALT, 'g'}, {KEY_ENTER, 'e'}, {KEY_F10, 'f'},
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (names[i].code == code) return names[i].c;
    return '?';
}

/* events: "C" CapsLock, "a" "k" "l" "x" letters, "s" Shift, "c" Ctrl, "g" AltGr, "e" Enter, "f" F10; each as
 * +key (down), -key (up), *key (auto-repeat), separated by spaces */
static out_t run(kbd_hyper_t *h, const char *events) {
    static const struct { char c; uint16_t code; } keys[] = {
        {'C', KEY_CAPSLOCK}, {'a', KEY_A}, {'k', KEY_K}, {'l', KEY_L}, {'x', KEY_X}, {'s', KEY_LEFTSHIFT},
        {'c', KEY_LEFTCTRL}, {'g', KEY_RIGHTALT}, {'e', KEY_ENTER}, {'f', KEY_F10},
    };
    out_t  o = {0};
    size_t n = 0;

    for (const char *p = events; *p; p++) {
        if (*p == ' ') continue;
        int      state = *p == '+' ? 1 : *p == '-' ? 0 : 2;
        uint16_t code  = 0;

        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
            if (keys[i].c == p[1]) code = keys[i].code;
        p++;
        kbd_hyper_action_t action = HYPER_NONE;
        switch (kbd_hyper_key(h, code, state, &action)) {
            case HYPER_PASS:
                if (n + 2 < sizeof(o.typed)) {
                    char c = name(code);
                    o.typed[n++] = state == 2 ? 'r' : state == 1 ? (char)(c - 32) : c;
                }
                break;
            case HYPER_CAPS:
                o.caps++;
                break;
            case HYPER_CHORD:
                if (action == HYPER_LOG_QSO) o.log_qso++;
                else if (action == HYPER_NONE) o.none++;
                break;
            case HYPER_DROP:
                break;
        }
    }
    o.typed[n] = 0;
    return o;
}

static void expect(const char *events, const char *typed, int caps, int log_qso, int none, const char *what) {
    kbd_hyper_t h = {0};
    out_t       o = run(&h, events);
    char        line[200];

    snprintf(line, sizeof(line), "%s (%s): typed \"%s\" want \"%s\", Caps Lock %d want %d, quick log %d want %d, "
             "no action %d want %d", what, events, o.typed, typed, o.caps, caps, o.log_qso, log_qso, o.none, none);
    check(!strcmp(o.typed, typed) && o.caps == caps && o.log_qso == log_qso && o.none == none, line);
    check(!h.held, "CapsLock still held after the case");
    for (size_t i = 0; i < sizeof(h.taken) / sizeof(h.taken[0]); i++)
        if (h.taken[i]) {
            check(false, "a chord key still marked down after the case");
            break;
        }
}

static void table(void) {
    check(kbd_hyper_action(KEY_L) == HYPER_LOG_QSO, "CapsLock+L is not the quick QSO log");
    for (uint16_t code = 0; code < KEY_CNT; code++)
        if (code != KEY_L && kbd_hyper_action(code) != HYPER_NONE) {
            char line[64];
            snprintf(line, sizeof(line), "CapsLock+key %u has an action: only L has one", code);
            check(false, line);
        }
}

int main(void) {
    table();

    expect("+a -a +x -x", "AaXx", 0, 0, 0, "typing without CapsLock");
    expect("+C -C", "", 1, 0, 0, "CapsLock tapped alone");
    expect("+C *C *C -C", "", 1, 0, 0, "CapsLock held alone, repeating");
    expect("+C -C +a -a +C -C", "Aa", 2, 0, 0, "Caps Lock on, a letter, off");
    expect("+C +l -l -C", "", 0, 1, 0, "CapsLock+L");
    expect("+C +l *l *l -l -C", "", 0, 1, 0, "CapsLock+L held, repeating");
    expect("+C +l -C -l", "", 0, 1, 0, "CapsLock let go before L");
    expect("+C +l -C *l -l +a -a", "Aa", 0, 1, 0, "L's repeat and release after CapsLock dropped, then typing");
    expect("+C +l -l +l -l -C", "", 0, 2, 0, "CapsLock+L twice in one hold");
    expect("+C +k -k -C", "", 0, 0, 1, "CapsLock+K: no action, nothing typed");
    expect("+C +e -e +f -f -C", "", 0, 0, 2, "CapsLock+Enter, CapsLock+F10: nothing");
    expect("+C +s +l -l -s -C", "Ss", 0, 1, 0, "CapsLock+Shift+L: Shift goes through");
    expect("+s +C +l -l -C -s", "Ss", 0, 1, 0, "Shift held, then CapsLock+L");
    expect("+C +c -c -C", "Cc", 1, 0, 0, "CapsLock with Ctrl only is a tap");
    expect("+C +g +l -l -g -C", "Gg", 0, 1, 0, "AltGr with the chord goes through");
    expect("+a +C *a -a -C", "Ara", 1, 0, 0, "a key down before CapsLock repeats and goes up");
    expect("+a +C +l -a -l -C", "Aa", 0, 1, 0, "a key down before, then CapsLock+L");
    expect("-C -l +a -a", "lAa", 0, 0, 0, "releases of keys never seen down: CapsLock's dropped, the others on");

    /* The state carries over between events of different calls, as key_input.c keeps it */
    kbd_hyper_t h = {0};
    out_t       a = run(&h, "+C +l"), b = run(&h, "-l -C +a -a");
    check(a.log_qso == 1 && !b.caps && !strcmp(b.typed, "Aa"), "a chord over two reads of the keyboard");

    printf("%d cases, %d wrong\n", cases, bad);
    return bad != 0;
}
