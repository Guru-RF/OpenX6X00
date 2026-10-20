/* Runtime font choice: the code keeps using &sony_N; for Roboto/DejaVu/Arial the contents of each
 * sony_N are replaced by the alt font before the UI is built. The alt fonts are ASCII only
 * (lv_font_conv --bpp 4 --no-compress --range 32-126 --format lvgl, size two steps below
 * Sony's; Arial: tools/font_gen.py, about 0.8 x Sony's size so two-line button labels fit);
 * icon glyphs come from the untouched Sony copy through the fallback. */
#include <stdbool.h>
#include "lvgl/lvgl.h"
#include "font_select.h"

#define SIZES(X) X(14) X(16) X(18) X(20) X(22) X(24) X(26) X(28) X(30) \
                 X(32) X(34) X(36) X(38) X(40) X(42) X(44) X(60)

#define DECL(n) extern lv_font_t sony_##n; extern const lv_font_t roboto_##n, dejavu_##n, arial_##n, rcond_##n;
SIZES(DECL)

#define SONY(n) &sony_##n,
#define ROBOTO(n) &roboto_##n,
#define DEJAVU(n) &dejavu_##n,
#define ARIAL(n) &arial_##n,
#define RCOND(n) &rcond_##n,
static lv_font_t *const sony[] = { SIZES(SONY) };
static const lv_font_t *const roboto[] = { SIZES(ROBOTO) };
static const lv_font_t *const dejavu[] = { SIZES(DEJAVU) };
static const lv_font_t *const arial[] = { SIZES(ARIAL) };
static const lv_font_t *const rcond[] = { SIZES(RCOND) };
#define N_FONTS (sizeof(sony) / sizeof(sony[0]))

static lv_font_t orig[N_FONTS];   /* Sony's, as built */
static bool saved;

int buttons_h = 54;  /* = BUTTONS_H_MIN */

/* Again on a theme change: Sony's back first, then the choice */
void fonts_apply(uint8_t choice) {
    for (unsigned i = 0; i < N_FONTS; i++) {
        if (!saved) orig[i] = *sony[i];
        *sony[i] = orig[i];
    }
    saved = true;
    buttons_h = 54;
    if (choice != FONT_ROBOTO && choice != FONT_DEJAVU && choice != FONT_ARIAL && choice != FONT_RCOND) return;
    if (choice == FONT_DEJAVU) buttons_h = 62;
    const lv_font_t *const *alt = choice == FONT_ROBOTO ? roboto : choice == FONT_DEJAVU ? dejavu :
                                   choice == FONT_ARIAL ? arial : rcond;

    for (unsigned i = 0; i < N_FONTS; i++) {
        *sony[i] = *alt[i];
        sony[i]->fallback = &orig[i];
    }
}

/* The sony_N `steps` sizes smaller (sony_60 stays): for screens that VISION's scale makes too big */
const lv_font_t *font_down(const lv_font_t *f, int steps) {
    for (int i = 0; i + 1 < (int)N_FONTS; i++)
        if (sony[i] == f) return sony[i - steps > 0 ? i - steps : 0];
    return f;
}

/* VISION: every sony_N drawn `steps` sizes bigger (sony_20 as sony_26 with 3), up to sony_44; sony_60 stays.
 * Call after fonts_apply(), before the UI is built */
void fonts_scale(int steps) {
    static lv_font_t was[N_FONTS];

    for (unsigned i = 0; i < N_FONTS; i++) {
        if (!saved) orig[i] = *sony[i];   /* without fonts_apply() first (the UI harness) */
        was[i] = *sony[i];
    }
    saved = true;
    for (unsigned i = 0; i + 1 < N_FONTS; i++) {
        unsigned to = i + steps < N_FONTS - 1 ? i + steps : N_FONTS - 2;
        *sony[i]    = was[to];
        /* what the bigger one lacks (the symbols: only Sony's 24 and 38 have them) from Sony's
         * size the code asked for: an empty box otherwise (the knobs' arrows) */
        sony[i]->fallback = &orig[i];
    }
}
