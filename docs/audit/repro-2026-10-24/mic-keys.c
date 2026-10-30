/* Issue #56: the hand mic's keys (src/mic_keys.c, linked whole) with the GUI's queue, timers and log stood in for.
 * Every code the base reports goes into the log once, with the key it is taken for or "unknown", so a press of each
 * key of another mic gives its codes; none while a text window (a WiFi password) is open. Then each key of both
 * mics, the X6100's and the X6200's H-22 (its codes read off an X6200 with base 1.0.8), against what it is to do on
 * the main screen: built as is the X6100's GUI (./mic-keys), with -DBOARD_X6200 the X6200's (./mic-keys-x6200).
 * The X6100 keeps its mic's table; the X6200 knows the H-22's codes besides. */
#include "src/gui_events.h"
#include "src/key_input.h"
#include "src/mic_keys.h"
#include "src/screen_light.h"
#include "src/text_popup.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t    EVENT_HKEY = 0x1000;
lv_group_t *keyboard_group;

static bool          editing = true;    /* the main screen holds the keyboard group in edit mode */
static bool          text_window;       /* a text window open (the WiFi password) */
static char          did[512], logged[512];
static int           n_logged, n_unknown;
static lv_timer_cb_t hold_cb;           /* the hold timer mic_keys.c started, until it fires or is deleted */
static int           timer;

static void note(char *b, size_t size, const char *text) {
    snprintf(b + strlen(b), size - strlen(b), "%s%s", *b ? ", " : "", text);
}

static const char *key_name(uint32_t key) {
    static char b[16];

    switch (key) {
        case HKEY_CE:           return "CE";
        case HKEY_UP:           return "UP";
        case HKEY_DOWN:         return "DOWN";
        case HKEY_SPCH:         return "SPCH";
        case HKEY_TUNER:        return "TUNER";
        case HKEY_XFC:          return "XFC";
        case HKEY_VM:           return "V/M";
        case HKEY_NW:           return "NW";
        case HKEY_F1:           return "F1";
        case HKEY_F2:           return "F2";
        case HKEY_F3:           return "F3";
        case HKEY_MODE:         return "MODE";
        case HKEY_FIL:          return "FIL";
        case HKEY_GENE:         return "GENE";
        case HKEY_FINP:         return "F-INP";
        case HKEY_VOL_UP:       return "VOL+";
        case HKEY_VOL_DOWN:     return "VOL-";
        case HKEY_AB:           return "A/B";
        case HKEY_BAND_UP:      return "BAND UP";
        case HKEY_BAND_DOWN:    return "BAND DOWN";
    }
    snprintf(b, sizeof(b), key >= ' ' && key < 0x7F ? "%c" : "0x%X", (unsigned)key);
    return b;
}

void _lv_log_add(lv_log_level_t level, const char *file, int line, const char *func, const char *format, ...) {
    char    b[128];
    va_list ap;

    va_start(ap, format);
    vsnprintf(b, sizeof(b), format, ap);
    va_end(ap);
    note(logged, sizeof(logged), b);
    n_logged++;
    n_unknown += strstr(b, "(unknown)") != NULL;
}

lv_disp_t  *lv_disp_get_default(void) { return NULL; }
lv_obj_t   *lv_disp_get_scr_act(lv_disp_t *disp) { return NULL; }
bool        lv_group_get_editing(const lv_group_t *group) { return editing; }
void        lv_timer_del(lv_timer_t *t) { hold_cb = NULL; }
void        lv_timer_set_repeat_count(lv_timer_t *t, int32_t count) {}
bool        backlight_tick() { return false; }
bool        textarea_window_is_open(void) { return text_window; }

lv_timer_t *lv_timer_create(lv_timer_cb_t cb, uint32_t period, void *data) {
    hold_cb = cb;
    return (lv_timer_t *)&timer;
}

bool textarea_window_type(uint32_t key) {
    char b[48];

    snprintf(b, sizeof(b), "typed %s into the text window", key_name(key));
    note(did, sizeof(did), b);
    return true;
}

void event_send(lv_obj_t *obj, lv_event_code_t code, void *param) {
    static const char *state[] = {"press", "release", "long", "long release"};
    event_hkey_t      *e       = param;
    char               b[32];

    snprintf(b, sizeof(b), "%s %s", state[e->state], key_name(e->key));
    note(did, sizeof(did), b);
    free(param);
}

void event_send_key(int32_t key) {
    char b[32];

    snprintf(b, sizeof(b), "typed %s", key_name(key));
    note(did, sizeof(did), b);
}

static void press(const char *where, uint32_t code) {
    *did = *logged = 0;
    hkey_put(code);
    hkey_put(0);
    printf("%s, 0x%02X: %s; log: %s\n", where, (unsigned)code, *did ? did : "nothing", *logged ? logged : "nothing");
}

/* Each key of a mic, and the key it is to be taken for on each radio (NULL: unknown, nothing happens). On the main
 * screen a key goes to it as a mic key (press, release), the digits and the dot too (the band on the key); CE and
 * F-INP are typed; the log names the key, as the press does */
typedef struct {
    uint32_t    code;
    const char *label;
    const char *x6100, *x6200;
} mic_key_t;

#define BOTH(s) s, s

static const mic_key_t x6100_mic[] = {
    {0xE1, "1", BOTH("1")},       {0xE2, "2", BOTH("2")},         {0xE4, "3", BOTH("3")},
    {0x61, "4", BOTH("4")},       {0x62, "5", BOTH("5")},         {0x64, "6", BOTH("6")},
    {0xA1, "7", BOTH("7")},       {0xA2, "8", BOTH("8")},         {0xA4, "9", BOTH("9")},
    {0x22, "0", BOTH("0")},       {0x21, ".", BOTH(".")},         {0x24, "CE", BOTH("CE")},
    {0x28, "F-INP", BOTH("F-INP")}, {0xC1, "SPCH", BOTH("SPCH")}, {0xC2, "TUNER", BOTH("TUNER")},
    {0xC4, "XFC", BOTH("XFC")},   {0x84, "UP", BOTH("UP")},       {0x04, "DOWN", BOTH("DOWN")},
    {0x81, "V/M", BOTH("V/M")},   {0x82, "NW", BOTH("NW")},       {0x48, "F1", BOTH("F1")},
    {0x44, "F2", BOTH("F2")},     {0xE8, "MODE", BOTH("MODE")},   {0x68, "FIL", BOTH("FIL")},
    {0xA8, "GENE", BOTH("GENE")},
};

/* The H-22 (#56): its codes from an X6200 with base 1.0.8, as ea4iln-debug read them off his radio */
static const mic_key_t h22[] = {
    {0xE1, "1", BOTH("1")},
    {0xE2, "2", BOTH("2")},
    {0x18, "3", NULL, "3"},
    {0x19, "4", NULL, "4"},
    {0x1A, "5", NULL, "5"},
    {0x1B, "6", NULL, "6"},
    {0x1C, "7", NULL, "7"},
    {0x1D, "8", NULL, "8"},
    {0x1E, "9", NULL, "9"},
    {0x17, "0", NULL, "0"},
    {0x1F, "*", NULL, "."},
    {0x28, "#", BOTH("F-INP")},
    {0x48, "F1", BOTH("F1")},
    {0x44, "F2", BOTH("F2")},
    {0x13, "F3", NULL, "F3"},
    {0x84, "arrow up", BOTH("UP")},
    {0x04, "arrow down", BOTH("DOWN")},
    {0x24, "HOME", BOTH("CE")},
    {0x14, "VOL+", NULL, "VOL+"},
    {0x15, "VOL-", NULL, "VOL-"},
    {0x16, "A/B", NULL, "A/B"},
    {0xE8, "MODE", BOTH("MODE")},
    {0x81, "V/M", BOTH("V/M")},
    {0x12, "BAND UP (Xiegu's app; never seen)", NULL, "BAND UP"},
    {0x11, "BAND DOWN (Xiegu's app; never seen)", NULL, "BAND DOWN"},
};

#ifdef BOARD_X6200
#define BOARD "X6200"
#define WANT(k) (k).x6200
#else
#define BOARD "X6100"
#define WANT(k) (k).x6100
#endif

static bool walk(const char *mic, const mic_key_t *k, size_t n) {
    size_t good = 0;

    editing = true;
    for (size_t i = 0; i < n; i++) {
        const char *key = WANT(k[i]);
        char        want[64], want_log[64];

        if (!key) {
            snprintf(want, sizeof(want), "nothing");
        } else if (strcmp(key, "CE") == 0 || strcmp(key, "F-INP") == 0) {
            snprintf(want, sizeof(want), "typed %s", key);
        } else {
            snprintf(want, sizeof(want), "press %s, release %s", key, key);
        }
        snprintf(want_log, sizeof(want_log), "hand mic: key 0x%02X (%s)", (unsigned)k[i].code, key ? key : "unknown");
        *did = *logged = 0;
        hkey_put(k[i].code);
        hkey_put(0);

        bool ok = strcmp(*did ? did : "nothing", want) == 0 && strcmp(logged, want_log) == 0;

        good += ok;
        printf("%s, %s %s, 0x%02X: %s; log: %s%s\n", BOARD, mic, k[i].label, (unsigned)k[i].code,
               *did ? did : "nothing", logged, ok ? "" : "  <-- not as it should be");
    }
    printf("%s, the %s: %zu of %zu keys as they should be\n", BOARD, mic, good, n);
    return good == n;
}

int main(void) {
    press("main screen", 0xE4);
    press("main screen", 0xE1);
    press("main screen", 0x48);
    press("main screen", 0x53);
    editing = false;
    press("a list focused", 0xE2);
    press("a list focused", 0x28);
    text_window = true;
    press("a text window open", 0xE1);
    press("a text window open", 0x53);
    text_window = false;

    n_logged = n_unknown = 0;
    for (uint32_t code = 1; code < 256; code++) {
        hkey_put(code);
        hkey_put(0);
    }
    printf("codes 0x01-0xFF: logged %d, known %d, unknown %d\n", n_logged, n_logged - n_unknown, n_unknown);

    bool all = walk("X6100 mic", x6100_mic, sizeof(x6100_mic) / sizeof(x6100_mic[0]));

    all &= walk("H-22", h22, sizeof(h22) / sizeof(h22[0]));

    /* Held: the hold timer's long press (a digit: its memory), and whether VOL+ is down, which its steps repeat
     * on in screen_main.c */
    editing = true;
    for (int i = 0; i < 2; i++) {
        uint32_t code = i ? 0x14 : 0x18;

        *did    = 0;
        hold_cb = NULL;
        hkey_put(code);

        bool down = hkey_down(HKEY_VOL_UP);

        if (hold_cb) {
            hold_cb(NULL);
        }
        hkey_put(0);
        printf("%s, 0x%02X held: %s; VOL+ down while held: %d, after: %d\n", BOARD, (unsigned)code,
               *did ? did : "nothing", down, hkey_down(HKEY_VOL_UP));
    }
    return all ? 0 : 1;
}
