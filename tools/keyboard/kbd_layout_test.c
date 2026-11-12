/* Host test of the keyboard's layouts (src/kbd_layout.c, #236): every layout of the list made by xkb from each xkb
 * data folder given (the PC's, and the radios' built target/ when XKB_ROOTS names them), the model each asks for
 * included, and what a few keys type in US, German, French and UK, Shift and the key left of Z too. Also the list's
 * labels as the dropdown gets them, and an index out of the list giving US. */
#include "kbd_layout.h"

#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

static int fails, checks;

#define SH KEY_LEFTSHIFT
#define AG KEY_RIGHTALT

#define CHECK(c, ...)                                   \
    do {                                                \
        checks++;                                       \
        if (!(c)) {                                     \
            fails++;                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

static struct xkb_keymap *make(struct xkb_context *ctx, const kbd_layout_t *l) {
    struct xkb_rule_names n = {.rules = NULL, .model = l->model, .layout = l->layout, .variant = NULL, .options = NULL};

    return xkb_keymap_new_from_names(ctx, &n, XKB_KEYMAP_COMPILE_NO_FLAGS);
}

/* What the key types, with a modifier held (Shift, AltGr) or none, as key_input.c's xkb gets it (evdev code + 8) */
static const char *typed(struct xkb_keymap *km, int code, int mod) {
    static char        buf[16];
    struct xkb_state *st = xkb_state_new(km);

    if (mod)
        xkb_state_update_key(st, mod + 8, XKB_KEY_DOWN);
    xkb_state_key_get_utf8(st, code + 8, buf, sizeof(buf));
    xkb_state_unref(st);
    return buf;
}

static int index_of(const char *layout) {
    for (int i = 0; i < KBD_LAYOUTS; i++)
        if (strcmp(kbd_layout_get(i)->layout, layout) == 0)
            return i;
    return -1;
}

static void keys(struct xkb_context *ctx, const char *root) {
    struct {
        const char *layout;
        int         code, mod;
        const char *want;
    } cases[] = {
        {"us", KEY_Y, 0, "y"},   {"us", KEY_Z, 0, "z"},     {"us", KEY_SLASH, 0, "/"}, {"us", KEY_2, SH, "@"},
        {"de", KEY_Y, 0, "z"},   {"de", KEY_Z, 0, "y"},     {"de", KEY_Z, SH, "Y"},    {"de", KEY_7, SH, "/"},
        {"de", KEY_SLASH, 0, "-"}, {"de", KEY_102ND, 0, "<"}, {"de", KEY_A, 0, "a"},   {"de", KEY_1, 0, "1"},
        {"de", KEY_Q, AG, "@"},
        {"fr", KEY_Q, 0, "a"},   {"fr", KEY_A, 0, "q"},     {"fr", KEY_W, 0, "z"},     {"fr", KEY_1, SH, "1"},
        {"gb", KEY_2, SH, "\""}, {"gb", KEY_BACKSLASH, 0, "#"},
        {"be", KEY_Q, 0, "a"},   {"ch", KEY_Y, 0, "z"},     {"cz", KEY_Y, 0, "z"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int                k  = index_of(cases[i].layout);
        struct xkb_keymap *km = k >= 0 ? make(ctx, kbd_layout_get(k)) : NULL;
        const char        *got = km ? typed(km, cases[i].code, cases[i].mod) : "(no keymap)";

        CHECK(strcmp(got, cases[i].want) == 0, "%s: %s key %d%s types \"%s\", not \"%s\"", root, cases[i].layout,
              cases[i].code, cases[i].mod == SH ? " with Shift" : cases[i].mod == AG ? " with AltGr" : "", got,
              cases[i].want);
        if (km)
            xkb_keymap_unref(km);
    }
}

static void root_check(const char *root) {
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES | XKB_CONTEXT_NO_ENVIRONMENT_NAMES);

    xkb_context_set_log_level(ctx, XKB_LOG_LEVEL_CRITICAL);
    CHECK(xkb_context_include_path_append(ctx, root), "%s: no xkb data there", root);
    for (int i = 0; i < KBD_LAYOUTS; i++) {
        struct xkb_keymap *km = make(ctx, kbd_layout_get(i));

        CHECK(km != NULL, "%s: xkb makes no keymap for %s (%s, %s)", root, kbd_layout_get(i)->label,
              kbd_layout_get(i)->layout, kbd_layout_get(i)->model);
        if (km)
            xkb_keymap_unref(km);
    }
    keys(ctx, root);
    xkb_context_unref(ctx);
}

int main(void) {
    const char *roots = getenv("XKB_ROOTS");
    char        list[2048], *save, *r;
    const char *opts = kbd_layout_options();
    int         lines = 1;

    /* The list */
    for (const char *p = opts; *p; p++)
        lines += *p == '\n';
    CHECK(lines == KBD_LAYOUTS, "%d labels, %d layouts", lines, KBD_LAYOUTS);
    const char *start = " US \n UK \n German (QWERTZ) \n";
    CHECK(strncmp(opts, start, strlen(start)) == 0, "labels start \"%.30s\"", opts);
    CHECK(opts[strlen(opts) - 1] == ' ', "no newline after the last label");
    CHECK(strcmp(kbd_layout_get(0)->layout, "us") == 0 && strcmp(kbd_layout_get(0)->model, "pc101") == 0,
          "the first is v0.3.9's US, pc101");
    CHECK(kbd_layout_get(-1) == kbd_layout_get(0) && kbd_layout_get(KBD_LAYOUTS) == kbd_layout_get(0),
          "out of the list: US");
    for (int i = 0; i < KBD_LAYOUTS; i++)
        for (int j = i + 1; j < KBD_LAYOUTS; j++)
            CHECK(strcmp(kbd_layout_get(i)->layout, kbd_layout_get(j)->layout) != 0, "%s twice",
                  kbd_layout_get(i)->layout);

    /* The xkb data: the PC's, and the radios' when given */
    snprintf(list, sizeof(list), "/usr/share/X11/xkb%s%s", roots ? " " : "", roots ? roots : "");
    for (r = strtok_r(list, " ", &save); r; r = strtok_r(NULL, " ", &save))
        root_check(r);

    printf("kbd_layout: %d of %d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
