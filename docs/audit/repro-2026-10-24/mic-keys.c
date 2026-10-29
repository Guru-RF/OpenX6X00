/* Issue #56: the hand mic's keys (src/mic_keys.c, linked whole) with the GUI's queue, timers and log stood in for.
 * Every code the base reports goes into the log once, with the X6100 mic's key it is taken for or "unknown", so a
 * press of each key of another mic (the X6200's H-22) gives its codes: ./mic-keys */
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

static bool editing = true;     /* the main screen holds the keyboard group in edit mode */
static char did[512], logged[512];
static int  n_logged, n_unknown;

static void note(char *b, size_t size, const char *text) {
    snprintf(b + strlen(b), size - strlen(b), "%s%s", *b ? ", " : "", text);
}

static const char *key_name(uint32_t key) {
    static char b[16];

    if (key < 0x80) {
        snprintf(b, sizeof(b), "%c", (char)key);
    } else {
        snprintf(b, sizeof(b), "%s", key == HKEY_F1 ? "F1" : key == HKEY_FINP ? "F-INP" : "other");
    }
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
lv_timer_t *lv_timer_create(lv_timer_cb_t cb, uint32_t period, void *data) { static int t; return (lv_timer_t *)&t; }
void        lv_timer_del(lv_timer_t *timer) {}
void        lv_timer_set_repeat_count(lv_timer_t *timer, int32_t count) {}
bool        backlight_tick() { return false; }
bool        textarea_window_is_open(void) { return false; }
bool        textarea_window_type(uint32_t key) { return true; }

void event_send(lv_obj_t *obj, lv_event_code_t code, void *param) {
    event_hkey_t *e = param;
    char          b[32];

    snprintf(b, sizeof(b), "%s %s", e->state == HKEY_PRESS ? "press" : "release", key_name(e->key));
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
    printf("%s, 0x%02X: %s; log: %s\n", where, (unsigned)code, *did ? did : "nothing", logged);
}

int main(void) {
    press("main screen", 0xE4);
    press("main screen", 0xE1);
    press("main screen", 0x48);
    press("main screen", 0x53);
    editing = false;
    press("a list focused", 0xE2);
    press("a list focused", 0x28);

    n_logged = n_unknown = 0;
    for (uint32_t code = 1; code < 256; code++) {
        hkey_put(code);
        hkey_put(0);
    }
    printf("codes 0x01-0xFF: logged %d, known %d, unknown %d\n", n_logged, n_logged - n_unknown, n_unknown);
    return 0;
}
