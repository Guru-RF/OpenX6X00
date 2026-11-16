/* Runtime font choice: the code keeps using &sony_N; for Roboto/DejaVu/Arial the contents of each
 * sony_N are replaced by the alt font before the UI is built. The alt fonts are ASCII only
 * (lv_font_conv --bpp 4 --no-compress --range 32-126 --format lvgl, size two steps below
 * Sony's; Arial: tools/font_gen.py, about 0.8 x Sony's size so two-line button labels fit);
 * icon glyphs come from the untouched Sony copy through the fallback.
 * The pixel family (1bit): Terminus Font's bitmap sizes at 1 bpp (tools/fonts/bdf2lvgl.py), chosen by
 * cap and line height, so several Sony sizes share one; Latin-1 too.
 * Behind every sony_N, before the stock font's copy (orig), the letters of Latin-1 (latin1_N, tools/fonts/mklatin.sh:
 * the capitals the CW decoder and the CW Keyboard show, Å Ä Ö Ü É Ñ, what the text boxes were given, the GUI's
 * languages), for the fonts that have none. Behind orig, when the language chosen asks for one (i18n_font(): Chinese),
 * a TrueType font drawn as needed by LVGL's reader (tiny_ttf): the ideographs at the size of the slot's capitals. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "lvgl/lvgl.h"
#include "font_select.h"
#include "../i18n.h"

#define SIZES(X) X(14) X(16) X(18) X(20) X(22) X(24) X(26) X(28) X(30) \
                 X(32) X(34) X(36) X(38) X(40) X(42) X(44) X(60)

#define DECL(n) extern lv_font_t sony_##n; extern const lv_font_t roboto_##n, dejavu_##n, arial_##n, rcond_##n, codec_##n, \
    latin1_##n;
SIZES(DECL)
extern const lv_font_t pixel_12, pixel_14, pixel_16, pixel_18, pixel_20, pixel_22, pixel_24, pixel_28, pixel_32,
    pixel_44;

#define SONY(n) &sony_##n,
#define ROBOTO(n) &roboto_##n,
#define DEJAVU(n) &dejavu_##n,
#define ARIAL(n) &arial_##n,
#define RCOND(n) &rcond_##n,
#define CODEC(n) &codec_##n,
#define LATIN1(n) &latin1_##n,
static lv_font_t *const sony[] = { SIZES(SONY) };
static const lv_font_t *const roboto[] = { SIZES(ROBOTO) };
static const lv_font_t *const dejavu[] = { SIZES(DEJAVU) };
static const lv_font_t *const arial[] = { SIZES(ARIAL) };
static const lv_font_t *const rcond[] = { SIZES(RCOND) };
/* sony_14 .. sony_60 */
static const lv_font_t *const pixel[] = {
    &pixel_12, &pixel_12, &pixel_14, &pixel_16, &pixel_18, &pixel_20, &pixel_20, &pixel_22, &pixel_24,
    &pixel_24, &pixel_28, &pixel_28, &pixel_32, &pixel_32, &pixel_32, &pixel_32, &pixel_44,
};
static const lv_font_t *const codec[] = { SIZES(CODEC) };
static const lv_font_t *const latin1[] = { SIZES(LATIN1) };   /* DejaVu Sans at dejavu_N's size, Latin-1 */
#define N_FONTS (sizeof(sony) / sizeof(sony[0]))
_Static_assert(sizeof(pixel) / sizeof(pixel[0]) == N_FONTS, "a pixel font for every size");

static lv_font_t orig[N_FONTS];   /* Sony's, as built */
static lv_font_t latin_link[N_FONTS];   /* the latin1_N behind each sony_N, orig behind it */
static bool saved;

/* ---- the language's own script (i18n.h) ---- */

/* A font file once read (mapped: the pages are read as glyphs are drawn) and its fonts by em size, kept: a label of
 * the screen before may point at one until it is gone, and a language seldom changes twice. A few files at most */
typedef struct {
    char           path[200];
    const uint8_t *data;
    size_t         len;
    lv_font_t     *by_em[128];
} script_t;

static script_t scripts[4];
static int      n_scripts;

static script_t *script_file(const char *path) {
    struct stat st;
    int         fd;
    void       *m;

    for (int i = 0; i < n_scripts; i++)
        if (!strcmp(scripts[i].path, path)) return scripts[i].data ? &scripts[i] : NULL;
    if (n_scripts == (int)(sizeof(scripts) / sizeof(scripts[0])) || strlen(path) >= sizeof(scripts[0].path)) {
        return NULL;
    }
    script_t *s = &scripts[n_scripts++];   /* a file that does not open is not tried again */

    strcpy(s->path, path);
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0) {
        LV_LOG_ERROR("the language's font %s: not found", path);
        return NULL;
    }
    m = fstat(fd, &st) == 0 && st.st_size > 12 ? mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    close(fd);
    if (m == MAP_FAILED) {
        LV_LOG_ERROR("the language's font %s: not read", path);
        return NULL;
    }
    /* one font, TrueType or OpenType outlines; a collection (.ttc) is not taken: its first font might be another
     * language's */
    const uint8_t *d = m;
    if (memcmp(d, "OTTO", 4) && memcmp(d, "\0\1\0\0", 4) && memcmp(d, "true", 4)) {
        LV_LOG_ERROR("the language's font %s: not a single TrueType or OpenType font", path);
        munmap(m, st.st_size);
        return NULL;
    }
    s->data = d;
    s->len  = st.st_size;
    return s;
}

/* The language's font for a slot whose capitals stand `cap` pixels tall: the em of DejaVu Sans with such capitals
 * (0.73 em), where an ideograph stands a little above the capitals, as in a printed page that mixes the two.
 * NULL: no language font, or it could not be read */
static lv_font_t *script_font(int cap) {
    const char *path = i18n_font();
    script_t   *s    = path ? script_file(path) : NULL;
    int         em   = (cap * 137 + 50) / 100;

    if (!s || em < 8) return NULL;
    if (em >= (int)(sizeof(s->by_em) / sizeof(s->by_em[0]))) em = sizeof(s->by_em) / sizeof(s->by_em[0]) - 1;
    if (!s->by_em[em]) {
        /* the drawn glyphs kept: about 300 of this size (a screen of Chinese) */
        s->by_em[em] = lv_tiny_ttf_create_data_ex(s->data, s->len, em, (size_t)300 * em * em);
        if (!s->by_em[em]) {
            LV_LOG_ERROR("the language's font %s at %d px: not made", s->path, em);
        }
    }
    return s->by_em[em];
}

/* The decoder's capitals behind sony_N i, as drawn now (another family, a bigger size under VISION): the latin1 size
 * whose capitals stand as tall as its own, by the A (dejavu_N's: latin1_N is the same face at the same size). The
 * families' capitals differ at one size (sony_24's 12 pixels, dejavu_24's 14): the slot's own latin1 would stand
 * out. The stock font's copy of the slot (orig) after it, for the symbols */
static void latin_behind(unsigned i) {
    lv_font_glyph_dsc_t g;
    int                 cap  = sony[i]->get_glyph_dsc(sony[i], &g, 'A', 0) ? g.box_h : 0;
    unsigned            best = i;
    int                 off  = -1;     /* best's capitals from the font's, in pixels */

    for (unsigned k = 0; cap && k < N_FONTS; k++) {
        int d = dejavu[k]->get_glyph_dsc(dejavu[k], &g, 'A', 0) ? LV_ABS(g.box_h - cap) : -1;

        if (d >= 0 && (off < 0 || d < off)) {
            best = k;
            off  = d;
        }
    }
    latin_link[i]          = *latin1[best];
    latin_link[i].fallback = &orig[i];
    orig[i].fallback       = script_font(cap ? cap : sony[i]->line_height / 2);
    sony[i]->fallback      = &latin_link[i];
}

int buttons_h = 54;  /* = BUTTONS_H_MIN */

/* Again on a theme change: Sony's back first, then the choice */
void fonts_apply(uint8_t choice) {
    for (unsigned i = 0; i < N_FONTS; i++) {
        if (!saved) orig[i] = *sony[i];
        *sony[i] = orig[i];
    }
    saved = true;
    buttons_h = 54;
    if (choice != FONT_ROBOTO && choice != FONT_DEJAVU && choice != FONT_ARIAL && choice != FONT_RCOND &&
        choice != FONT_PIXEL && choice != FONT_CODEC) {
        for (unsigned i = 0; i < N_FONTS; i++) latin_behind(i);
        return;
    }
    if (choice == FONT_DEJAVU) buttons_h = 62;
    const lv_font_t *const *alt = choice == FONT_ROBOTO ? roboto : choice == FONT_DEJAVU ? dejavu :
                                   choice == FONT_ARIAL ? arial : choice == FONT_PIXEL ? pixel :
                                   choice == FONT_CODEC ? codec : rcond;

    for (unsigned i = 0; i < N_FONTS; i++) {
        *sony[i] = *alt[i];
        latin_behind(i);
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
         * size the code asked for: an empty box otherwise (the knobs' arrows); the decoder's capitals
         * at the bigger one's size before that */
        latin_behind(i);
    }
}
