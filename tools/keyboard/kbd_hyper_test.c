/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of CapsLock as the keyboard's hyper key (src/kbd_hyper.c; issue #175). Key events go in as evdev
 *  sends them, and come out as key_input.c uses them: the keys typed (passed on to xkb and LVGL), the times Caps
 *  Lock turned, the chords' actions. CapsLock tapped alone is Caps Lock; held, a key with it is a chord, never
 *  typed, once per press (its repeats and release dropped, whichever goes up first), and Caps Lock stays as it was.
 *  The table: L log a QSO, K the dial lock, P a screenshot, S Settings, F the frequency box, Up and Down the band,
 *  Space PTT, 1-8 the CW messages and JTTY's macros; the other keys (the F keys, PgUp and PgDn too) do nothing with
 *  it. Keys pressed before CapsLock, and Shift, Ctrl, Alt and Meta, go through.
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

/* The keys of the cases, a character each */
static const struct {
    char     c;
    uint16_t code;
} keys[] = {
    {'C', KEY_CAPSLOCK}, {'a', KEY_A},        {'k', KEY_K},        {'l', KEY_L},          {'x', KEY_X},
    {'q', KEY_Q},        {'p', KEY_P},        {'s', KEY_S},        {'f', KEY_F},          {'1', KEY_1},
    {'2', KEY_2},        {'3', KEY_3},        {'4', KEY_4},        {'5', KEY_5},          {'6', KEY_6},
    {'7', KEY_7},        {'8', KEY_8},        {'9', KEY_9},        {'0', KEY_0},          {'^', KEY_UP},
    {'v', KEY_DOWN},     {'<', KEY_LEFT},     {'_', KEY_SPACE},    {'S', KEY_LEFTSHIFT},  {'R', KEY_LEFTCTRL},
    {'G', KEY_RIGHTALT}, {'E', KEY_ENTER},    {'U', KEY_PAGEUP},   {'D', KEY_PAGEDOWN},   {'O', KEY_SCROLLLOCK},
    {'P', KEY_SYSRQ},    {'!', KEY_F1},       {'@', KEY_F5},       {'#', KEY_F9},         {'$', KEY_F10},
};

static uint16_t code_of(char c) {
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (keys[i].c == c) return keys[i].code;
    printf("no key '%c' in the test\n", c);
    return 0;
}

static char name_of(uint16_t code) {
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (keys[i].code == code) return keys[i].c;
    return '?';
}

/* An action as a character: what main_screen_chord would do */
static char action_name(kbd_hyper_action_t a) {
    switch (a) {
        case HYPER_NONE:        return '.';
        case HYPER_LOG_QSO:     return 'L';
        case HYPER_LOCK:        return 'K';
        case HYPER_SCREENSHOT:  return 'P';
        case HYPER_SETTINGS:    return 'S';
        case HYPER_FREQ:        return 'F';
        case HYPER_BAND_UP:     return '^';
        case HYPER_BAND_DOWN:   return 'v';
        case HYPER_PTT:         return '_';
        default:
            if (a >= HYPER_KEY_1 && a <= HYPER_KEY_8) return (char)('1' + a - HYPER_KEY_1);
            return '?';
    }
}

/* What key_input.c makes of the events */
typedef struct {
    char typed[128];    /* the events passed on: +k down, -k up, *k a repeat */
    int  caps;          /* Caps Lock turned */
    char chords[64];    /* the chords' actions, a character each (action_name) */
} out_t;

/* events: +k (down), -k (up), *k (auto-repeat) for each key k of keys[], separated by spaces */
static out_t run(kbd_hyper_t *h, const char *events) {
    out_t  o = {0};
    size_t t = 0, c = 0;

    for (const char *p = events; *p; p++) {
        if (*p == ' ') continue;
        int      state = *p == '+' ? 1 : *p == '-' ? 0 : 2;
        uint16_t code  = code_of(p[1]);

        p++;
        kbd_hyper_action_t action = HYPER_NONE;
        switch (kbd_hyper_key(h, code, state, &action)) {
            case HYPER_PASS:
                if (t + 3 < sizeof(o.typed)) {
                    o.typed[t++] = state == 1 ? '+' : state == 0 ? '-' : '*';
                    o.typed[t++] = name_of(code);
                }
                break;
            case HYPER_CAPS:
                o.caps++;
                break;
            case HYPER_CHORD:
                if (c + 2 < sizeof(o.chords)) o.chords[c++] = action_name(action);
                break;
            case HYPER_DROP:
                break;
        }
    }
    return o;
}

static void expect(const char *events, const char *typed, int caps, const char *chords, const char *what) {
    kbd_hyper_t h = {0};
    out_t       o = run(&h, events);
    char        line[300];

    snprintf(line, sizeof(line), "%s (%s): typed \"%s\" want \"%s\", Caps Lock %d want %d, chords \"%s\" want \"%s\"",
             what, events, o.typed, typed, o.caps, caps, o.chords, chords);
    check(!strcmp(o.typed, typed) && o.caps == caps && !strcmp(o.chords, chords), line);
    check(!h.held, "CapsLock still held after the case");
    for (size_t i = 0; i < sizeof(h.taken) / sizeof(h.taken[0]); i++)
        if (h.taken[i]) {
            check(false, "a chord key still marked down after the case");
            break;
        }
}

/* Every chord in the table, and no other key with an action */
static void table(void) {
    static const struct {
        uint16_t           code;
        kbd_hyper_action_t action;
        const char        *name;
    } want[] = {
        {KEY_L, HYPER_LOG_QSO, "L"},     {KEY_K, HYPER_LOCK, "K"},           {KEY_P, HYPER_SCREENSHOT, "P"},
        {KEY_S, HYPER_SETTINGS, "S"},    {KEY_F, HYPER_FREQ, "F"},           {KEY_UP, HYPER_BAND_UP, "Up"},
        {KEY_DOWN, HYPER_BAND_DOWN, "Down"}, {KEY_SPACE, HYPER_PTT, "Space"}, {KEY_1, HYPER_KEY_1, "1"},
        {KEY_2, HYPER_KEY_2, "2"},       {KEY_3, HYPER_KEY_3, "3"},          {KEY_4, HYPER_KEY_4, "4"},
        {KEY_5, HYPER_KEY_5, "5"},       {KEY_6, HYPER_KEY_6, "6"},          {KEY_7, HYPER_KEY_7, "7"},
        {KEY_8, HYPER_KEY_8, "8"},
    };
    char line[96];
    int  with = 0;

    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        snprintf(line, sizeof(line), "CapsLock+%s: action %d, want %d", want[i].name, kbd_hyper_action(want[i].code),
                 want[i].action);
        check(kbd_hyper_action(want[i].code) == want[i].action, line);
    }
    for (uint16_t code = 0; code < KEY_CNT; code++)
        with += kbd_hyper_action(code) != HYPER_NONE;
    snprintf(line, sizeof(line), "%d keys have a chord, want %zu", with, sizeof(want) / sizeof(want[0]));
    check(with == (int)(sizeof(want) / sizeof(want[0])), line);
    /* Gone: the F keys and PgUp/PgDn have no chord, nor Q (L logs), 9, 0, the left arrow, Enter */
    static const uint16_t none[] = {KEY_F1, KEY_F4, KEY_F5, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12, KEY_PAGEUP,
                                    KEY_PAGEDOWN, KEY_9, KEY_0, KEY_LEFT, KEY_RIGHT, KEY_ENTER, KEY_A, KEY_KP1,
                                    KEY_Q};
    for (size_t i = 0; i < sizeof(none) / sizeof(none[0]); i++) {
        snprintf(line, sizeof(line), "CapsLock+key %u has an action", none[i]);
        check(kbd_hyper_action(none[i]) == HYPER_NONE, line);
    }
}

int main(void) {
    table();

    /* Caps Lock itself, and typing */
    expect("+a -a +x -x", "+a-a+x-x", 0, "", "typing without CapsLock");
    expect("+C -C", "", 1, "", "CapsLock tapped alone");
    expect("+C *C *C -C", "", 1, "", "CapsLock held alone, repeating");
    expect("+C -C +a -a +C -C", "+a-a", 2, "", "Caps Lock on, a letter, off");
    expect("+l -l +k -k +p -p +s -s +f -f +_ -_ +1 -1", "+l-l+k-k+p-p+s-s+f-f+_-_+1-1", 0, "",
           "the chord keys alone type as usual");
    expect("+^ -^ +v -v", "+^-^+v-v", 0, "", "the arrows alone go through");

    /* Each chord, in both release orders */
    static const char chord_keys[] = "lkpsf^v_12345678";
    static const char chord_want[] = "LKPSF^v_12345678";
    for (size_t i = 0; chord_keys[i]; i++) {
        char ev[64], want[2] = {chord_want[i], 0}, what[64];

        snprintf(ev, sizeof(ev), "+C +%c -%c -C", chord_keys[i], chord_keys[i]);
        snprintf(what, sizeof(what), "CapsLock+%c, the key up first", chord_keys[i]);
        expect(ev, "", 0, want, what);
        snprintf(ev, sizeof(ev), "+C +%c -C -%c +a -a", chord_keys[i], chord_keys[i]);
        snprintf(what, sizeof(what), "CapsLock+%c, CapsLock up first", chord_keys[i]);
        expect(ev, "+a-a", 0, want, what);
        snprintf(ev, sizeof(ev), "+C +%c *%c *%c *%c -%c -C", chord_keys[i], chord_keys[i], chord_keys[i],
                 chord_keys[i], chord_keys[i]);
        snprintf(what, sizeof(what), "CapsLock+%c held, repeating: once", chord_keys[i]);
        expect(ev, "", 0, want, what);
        snprintf(ev, sizeof(ev), "+C +%c -C *%c *%c -%c", chord_keys[i], chord_keys[i], chord_keys[i],
                 chord_keys[i]);
        snprintf(what, sizeof(what), "CapsLock+%c, repeats after CapsLock went up", chord_keys[i]);
        expect(ev, "", 0, want, what);
    }

    /* More chords in one hold, and held together */
    expect("+C +l -l +l -l -C", "", 0, "LL", "CapsLock+L twice in one hold");
    expect("+C +^ -^ +^ -^ +^ -^ -C", "", 0, "^^^", "CapsLock+Up three times: three bands");
    expect("+C +^ *^ *^ *^ -^ +v *v -v -C", "", 0, "^v", "CapsLock+Up then Down held: one band each");
    expect("+C +_ -_ +_ -_ -C", "", 0, "__", "CapsLock+Space twice: on, off");
    expect("+C +_ -C +C -_ -C", "", 1, "_", "Space held over a second CapsLock tap: one chord, the tap is Caps Lock");
    expect("+C +1 +2 -1 -2 -C", "", 0, "12", "CapsLock+1 and 2 overlapping");
    expect("+C +k +l -C -l -k", "", 0, "KL", "two chord keys, CapsLock up first, then both");
    expect("+C +q -q -C", "", 0, ".", "CapsLock+Q: no action, nothing typed");
    expect("+C +9 -9 +0 -0 +< -< +E -E -C", "", 0, "....", "CapsLock+9, 0, Left, Enter: nothing");

    /* The F keys and PgUp/PgDn: no chord, and alone they go on as any key (kbd_keymap gives them nothing) */
    expect("+C +! -! +@ -@ +# -# +$ -$ +U -U +D -D -C", "", 0, "......",
           "CapsLock with F1, F5, F9, F10, PgUp, PgDn: dropped, no action");
    expect("+! -! +$ -$ +U -U +D -D +O -O +P -P", "+!-!+$-$+U-U+D-D+O-O+P-P", 0, "",
           "F1, F10, PgUp, PgDn, Scroll Lock, Print Screen alone go through");
    expect("+C +O -O +P -P -C", "", 0, "..", "CapsLock with Scroll Lock, Print Screen: chords, dropped");

    /* Modifiers, and keys down before CapsLock */
    expect("+C +S +l -l -S -C", "+S-S", 0, "L", "CapsLock+Shift+L: Shift goes through");
    expect("+S +C +l -l -C -S", "+S-S", 0, "L", "Shift held, then CapsLock+L");
    expect("+C +R -R -C", "+R-R", 1, "", "CapsLock with Ctrl only is a tap");
    expect("+C +G +k -k -G -C", "+G-G", 0, "K", "AltGr with the chord goes through");
    expect("+a +C *a -a -C", "+a*a-a", 1, "", "a key down before CapsLock repeats and goes up");
    expect("+a +C +l -a -l -C", "+a-a", 0, "L", "a key down before, then CapsLock+L");
    expect("+_ +C *_ -_ -C", "+_*_-_", 1, "", "Space down before CapsLock: typed, repeats, no PTT");
    expect("+^ +C *^ +v -^ -v -C", "+^*^-^", 0, "v", "Up held before CapsLock, then CapsLock+Down");
    expect("-C -l +a -a", "-l+a-a", 0, "", "releases of keys never seen down: CapsLock's dropped, the others on");

    /* The state carries over between events of different calls, as key_input.c keeps it */
    kbd_hyper_t h = {0};
    out_t       a = run(&h, "+C +l"), b = run(&h, "-l -C +a -a");
    check(!strcmp(a.chords, "L") && !b.caps && !strcmp(b.typed, "+a-a"), "a chord over two reads of the keyboard");

    printf("%d cases, %d wrong\n", cases, bad);
    return bad != 0;
}
