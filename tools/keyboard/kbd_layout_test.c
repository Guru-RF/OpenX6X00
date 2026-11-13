/* Host test of the keyboard's layouts (src/kbd_layout.c, #236): every layout of the list made by xkb from each xkb
 * data folder given (the PC's, and the radios' built target/ when XKB_ROOTS names them), the model each asks for
 * included, and what a few keys type in US, German, French and UK, Shift and the key left of Z too. Also the list's
 * labels as the dropdown gets them, and an index out of the list giving US. #254: with Caps Lock on, the French and
 * Belgian number row's digits (kbd_layout_digits_row), its own characters with Shift, AltGr's and the letters as xkb
 * has them, and only those two layouts asking for it. */
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

/* #254: what kbd_layout_digits_row gives with Caps Lock on, as key_input.c's xkb has it, a modifier held or none;
 * "" for 0 (xkb's own character stays) */
static const char *caps_typed(struct xkb_keymap *km, int code, int mod) {
    static char        buf[8];
    struct xkb_state *st = xkb_state_new(km);
    uint32_t          c;

    xkb_state_update_key(st, KEY_CAPSLOCK + 8, XKB_KEY_DOWN);
    xkb_state_update_key(st, KEY_CAPSLOCK + 8, XKB_KEY_UP);
    if (mod)
        xkb_state_update_key(st, mod + 8, XKB_KEY_DOWN);
    c = kbd_layout_digits_row(st, code);
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &c, sizeof(c));
    xkb_state_unref(st);
    return buf;
}

static void digits(struct xkb_context *ctx, const char *root) {
    static const char *row = "1234567890";
    struct {
        const char *layout;
        int         code, mod;
        const char *want;
    } cases[] = {
        {"fr", KEY_2, SH, "\xc3\xa9"}, {"fr", KEY_1, SH, "&"}, {"fr", KEY_7, SH, "\xc3\xa8"}, {"fr", KEY_0, SH, "\xc3\xa0"},
        {"fr", KEY_2, AG, ""},   {"fr", KEY_Q, 0, ""},      {"fr", KEY_MINUS, 0, ""},  {"fr", KEY_SPACE, 0, ""},
        {"be", KEY_2, SH, "\xc3\xa9"}, {"be", KEY_6, SH, "\xc2\xa7"}, {"be", KEY_1, AG, ""},  {"be", KEY_Q, 0, ""},
    };

    for (int i = 0; i < KBD_LAYOUTS; i++) {
        const kbd_layout_t *l    = kbd_layout_get(i);
        bool                want = strcmp(l->layout, "fr") == 0 || strcmp(l->layout, "be") == 0;

        CHECK(l->caps_digits == want, "%s: Caps Lock %s the digits", l->layout, l->caps_digits ? "types" : "leaves");
    }
    /* The row: 1 to 0 with Caps Lock alone, where xkb types & é " ' ( and the others, É for é */
    for (const char *lay = "fr"; lay; lay = strcmp(lay, "fr") == 0 ? "be" : NULL) {
        struct xkb_keymap *km = make(ctx, kbd_layout_get(index_of(lay)));

        CHECK(km != NULL, "%s: no %s keymap", root, lay);
        if (!km)
            continue;
        for (int k = KEY_1; k <= KEY_0; k++) {
            char want[2] = {row[k - KEY_1], 0};
            const char *got = caps_typed(km, k, 0);

            CHECK(strcmp(got, want) == 0, "%s: %s key %d with Caps Lock types \"%s\", not \"%s\"", root, lay, k, got, want);
            CHECK(strcmp(typed(km, k, 0), want) != 0, "%s: %s key %d types its digit without Shift: not AZERTY", root, lay, k);
            if (k == KEY_1) {
                /* What the radio typed before: xkb with Caps Lock alone keeps the row's own & */
                struct xkb_state *st = xkb_state_new(km);
                char              own[8] = "";

                xkb_state_update_key(st, KEY_CAPSLOCK + 8, XKB_KEY_DOWN);
                xkb_state_update_key(st, KEY_CAPSLOCK + 8, XKB_KEY_UP);
                xkb_state_key_get_utf8(st, k + 8, own, sizeof(own));
                CHECK(strcmp(own, "&") == 0, "%s: %s xkb with Caps Lock types \"%s\" on the 1 key", root, lay, own);
                xkb_state_unref(st);
            }
        }
        xkb_keymap_unref(km);
    }
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct xkb_keymap *km  = make(ctx, kbd_layout_get(index_of(cases[i].layout)));
        const char        *got = km ? caps_typed(km, cases[i].code, cases[i].mod) : "(no keymap)";

        CHECK(strcmp(got, cases[i].want) == 0, "%s: %s key %d with Caps Lock%s gives \"%s\", not \"%s\"", root,
              cases[i].layout, cases[i].code, cases[i].mod == SH ? " and Shift" : cases[i].mod == AG ? " and AltGr" : "",
              got, cases[i].want);
        if (km)
            xkb_keymap_unref(km);
    }
    CHECK(kbd_layout_digits_row(NULL, KEY_1) == 0, "no state: 0");
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
    digits(ctx, root);
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
