/*
 * SSTV app: sending.
 *
 * TX opens a menu over the received picture (it stays visible, the call to
 * answer is often on it):
 *   CALL CQ          a picture with "CQ CQ SSTV" and our call, text colours
 *   REPLY TO <call>  a picture, a text from /mnt/sstv/tx_text, colours
 *   REPLAY TO <call> the same with a small copy of the received picture
 *   TX IMAGE ONLY    a picture as it is
 *   REPEAT LAST TX   the last complete transmission again, same mode
 *   SET TARGET CALLSIGN
 * The picture is fitted to the mode (TX Mode button) and the text drawn on
 * it here, so what is shown is exactly what goes out.  It is written to a
 * PNG, made into audio by sstv/sstv_tx.c and played by tx_player (keying,
 * power cap, level from ALC), after half a second of silence.
 * The menu choice, colours, target call and last picture are kept until the
 * firmware restarts, not saved.
 */
#include "app_sstv_int.h"
#include "board.h"

#include "audio.h"
#include "keyboard.h"
#include "msg.h"
#include "params/params.h"
#include "sstv/sstv_png.h"
#include "sstv/sstv_tx.h"
#include "styles.h"
#include "textarea_window.h"
#include "theme.h"
#include "tx_player.h"

#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* folders and the file handed to the encoder (other paths for the PC harness) */
#ifndef SSTV_TX_PICS_DIR
#define SSTV_TX_PICS_DIR BOARD_DATA "/sstv/tx_pics"
#define SSTV_TX_TEXT_DIR BOARD_DATA "/sstv/tx_text"
#define SSTV_TX_PNG      BOARD_DATA "/sstv-tx.png"
#endif

#define PREROLL_MS   500u          /* silence before the VIS: the TX path settles */
#define SETTLE_MS    300           /* a picture is loaded once the MFK rests this long */
#define LIST_X       (SSTV_PIC_X + SSTV_PIC_W + 10)
#define LIST_Y       (theme_shape.vision ? 84 : APP_FRAME_BODY_Y + 6)    /* VISION: under the header lines, in the column */
#define LIST_W       (APP_FRAME_X + APP_FRAME_W - 8 - LIST_X)
#define LIST_H       (theme_shape.vision ? DIALOG_H - 10 - LIST_Y : APP_FRAME_BODY_H - 12)
#define LIST_HEAD    32
#define HINT         "MFK: choose - press: confirm - Cancel TX: back"
#define TX_MAX       (320 * 256 * 3)

typedef enum { STEP_NONE = 0, STEP_PICTURE, STEP_MENU, STEP_TEXT, STEP_COLOURS } step_t;
typedef enum { MSG_CQ = 0, MSG_REPLY, MSG_REPLAY, MSG_IMAGE, MSG_REPEAT } msg_t;
typedef enum { ALIGN_LEFT, ALIGN_CENTRE, ALIGN_RIGHT } align_t;
typedef struct { uint8_t r, g, b; } rgb_t;

sstv_tx_state_t sstv_tx_state = SSTV_TX_OFF;

static step_t   step;
static msg_t    kind;
static bool     auto_after;              /* Auto RX was on before TX setup */
static lv_obj_t *box;                    /* the menu box with its title */
static lv_obj_t *menu, *pictures, *texts, *colours;   /* the lists */

/* the picture being prepared, at the mode's size, and with its text */
static uint8_t  picture[TX_MAX], framed[TX_MAX];
static unsigned pic_w, pic_h;
static char     pic_name[256];
/* the last complete transmission */
static uint8_t     last[TX_MAX];
static unsigned    last_w, last_h;
static sstv_mode_t last_mode = SSTV_SCOTTIE1;
static bool        last_ok;
static char        last_file[256];
/* the received picture while the menus use the picture area */
static lv_color_t rx_copy[SSTV_PIC_PIXELS];
static bool       rx_copy_ok;

static char   **pic_files, **text_files;
static size_t   pic_count, text_count;
static int      pic_shown = -1, pic_wanted = -1;
static uint64_t pic_wanted_at;
static int      menu_row, colour_row, colour_set;
static bool     text_redraw, colour_redraw;
static uint32_t colour_moved_at;
static char     text_middle[256], text_bottom[128];
static char     target[32];
static bool     kbd_open;
static lv_obj_t *keys;
static uint32_t kbd_lower_until;

/* on the air (tx_thread) */
static pthread_t        thread;
static bool             thread_on;
static volatile bool    running, stop, keyed;
static volatile int     result;          /* 1 sent, -1 stopped, < -1 error */
static volatile int     progress;
static sstv_mode_t      on_air = SSTV_ROBOT8;
static struct timespec  key_time;
static volatile bool    key_time_ok;

static void menu_open(void);
static void pictures_open(void);
static void texts_open(void);
static void colours_open(void);
static void draw_text_picture(void);
static void cq_picture(bool text);
static bool on_air_start(sstv_mode_t mode);

static uint64_t ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static uint64_t ms_since(const struct timespec *t0)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)(((int64_t)t.tv_sec - t0->tv_sec) * 1000 + ((int64_t)t.tv_nsec - t0->tv_nsec) / 1000000);
}

static int list_row(lv_obj_t *t)
{
    uint16_t row = LV_TABLE_CELL_NONE, col = LV_TABLE_CELL_NONE;
    lv_table_get_selected_cell(t, &row, &col);
    return row == LV_TABLE_CELL_NONE ? -1 : (int16_t)row;
}

/* This LVGL has no lv_table_set_selected_cell(): the list is moved down
 * with the keys the MFK sends, which also scrolls it */
static void list_go(lv_obj_t *t, int row)
{
    uint32_t key = LV_KEY_DOWN;
    int      r = list_row(t);
    if (r < 0) {
        lv_event_send(t, LV_EVENT_KEY, &key);
        r = list_row(t);
    }
    while (r >= 0 && r < row) {
        lv_event_send(t, LV_EVENT_KEY, &key);
        r = list_row(t);
    }
}

static void list_focus(lv_obj_t *t)
{
    lv_group_remove_obj(sstv_canvas);
    lv_group_add_obj(keyboard_group, t);
    lv_group_focus_obj(t);
    lv_group_set_editing(keyboard_group, true);
}

static void list_drop(lv_obj_t **t)
{
    if (!*t) return;
    lv_group_remove_obj(*t);
    lv_obj_del(*t);
    *t = NULL;
}

static void files_free(char ***files, size_t *count)
{
    for (size_t i = 0; i < *count; ++i) free((*files)[i]);
    free(*files);
    *files = NULL;
    *count = 0;
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int by_name_nocase(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

/* Visible files of dir ending in ext, sorted */
static void files_scan(const char *dir, const char *ext, bool nocase, char ***files, size_t *count)
{
    DIR           *d = opendir(dir);
    struct dirent *e;
    size_t         room = 0, el = strlen(ext);

    files_free(files, count);
    if (!d) return;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (e->d_name[0] == '.' || n <= el || strcmp(e->d_name + n - el, ext)) continue;   /* also macOS ._ files */
        if (*count == room) {
            char **p = realloc(*files, (room ? room * 2 : 32) * sizeof(char *));
            if (!p) break;
            *files = p;
            room = room ? room * 2 : 32;
        }
        if (((*files)[*count] = strdup(e->d_name))) ++*count;
    }
    closedir(d);
    qsort(*files, *count, sizeof(char *), nocase ? by_name_nocase : by_name);
}

/* ---- The menu box and its lists (look of the Modes menu) ---- */

static void box_close(void)
{
    if (box) {
        lv_obj_del(box);
        box = NULL;
    }
    if (sstv_frame.status) lv_label_set_text(sstv_frame.status, SSTV_HELP_LINE);
}

/* the hint goes in the status line */
static void box_open(const char *title, const char *hint)
{
    box_close();
    box = lv_obj_create(dialog_sstv->obj);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, LIST_X, LIST_Y);
    lv_obj_set_size(box, LIST_W, LIST_H);
    lv_obj_add_style(box, &popup_style, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *head = lv_label_create(box);
    lv_obj_set_style_text_font(head, &sony_20, 0);
    lv_obj_set_style_text_color(head, lv_color_hex(APP_FRAME_HEAD), 0);
    lv_label_set_text(head, title);
    lv_obj_set_pos(head, 20, 7);
    if (sstv_frame.status) lv_label_set_text(sstv_frame.status, hint);
}

static void row_look_cb(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *d = lv_event_get_draw_part_dsc(e);
    if (!d || d->part != LV_PART_ITEMS || !d->rect_dsc) return;

    bool on = d->id == (uint32_t)(uint16_t)list_row(lv_event_get_target(e));
    d->rect_dsc->bg_color = TC(sel_bg);
    d->rect_dsc->bg_opa = on ? LV_OPA_COVER : LV_OPA_TRANSP;
    d->rect_dsc->border_color = TC(sel_border);
    d->rect_dsc->border_opa = LV_OPA_COVER;
    d->rect_dsc->border_width = on ? 2 : 0;
    d->rect_dsc->border_side = LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM;
    d->rect_dsc->outline_width = 0;
    if (on && d->label_dsc) d->label_dsc->color = TC(sel_text);
}

static lv_obj_t *list_new(uint16_t rows)
{
    lv_obj_t *t = lv_table_create(dialog_sstv->obj);
    lv_obj_remove_style(t, NULL, LV_STATE_ANY | LV_PART_MAIN);
    lv_obj_set_pos(t, LIST_X + 2, LIST_Y + LIST_HEAD);
    lv_obj_set_size(t, LIST_W - 4, LIST_H - LIST_HEAD - 8);
    lv_obj_set_style_border_width(t, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_font(t, &sony_22, LV_PART_ITEMS);
    lv_obj_set_style_text_color(t, TC(text), LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(t, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(t, 18, LV_PART_ITEMS);
    lv_table_set_col_cnt(t, 1);
    lv_table_set_col_width(t, 0, LIST_W - 4);
    lv_table_set_row_cnt(t, rows);
    lv_obj_add_event_cb(t, sstv_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(t, row_look_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    return t;
}

static void status_text_show(bool on)
{
    if (!sstv_text) return;
    if (on)
        lv_obj_clear_flag(sstv_text, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(sstv_text, LV_OBJ_FLAG_HIDDEN);
}

/* ---- Text on the picture: our own 5x7 letters, drawn into the pixels that go out ---- */

static const char  font_chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ/";
static const char *const font[14] = {   /* 19 letters a row, 6 columns each */
    ".###. ..#.. .###. ##### ...#. ##### ..##. ##### .###. .###. .###. ####. .###. ###.. ##### ##### .###. #...# .###.",
    "#...# .##.. #...# ...#. ..##. #.... .#... ....# #...# #...# #...# #...# #...# #..#. #.... #.... #...# #...# ..#..",
    "#..## ..#.. ....# ..#.. .#.#. ####. #.... ...#. #...# #...# #...# #...# #.... #...# #.... #.... #.... #...# ..#..",
    "#.#.# ..#.. ...#. ...#. #..#. ....# ####. ..#.. .###. .#### #...# ####. #.... #...# ####. ####. #.### ##### ..#..",
    "##..# ..#.. ..#.. ....# ##### ....# #...# .#... #...# ....# ##### #...# #.... #...# #.... #.... #...# #...# ..#..",
    "#...# ..#.. .#... #...# ...#. #...# #...# .#... #...# ...#. #...# #...# #...# #..#. #.... #.... #...# #...# ..#..",
    ".###. .###. ##### .###. ...#. .###. .###. .#... .###. .##.. #...# ####. .###. ###.. ##### #.... .#### #...# .###.",
    "..### #...# #.... #...# #...# .###. ####. .###. ####. .#### ##### #...# #...# #...# #...# #...# ##### .....",
    "...#. #..#. #.... ##.## #...# #...# #...# #...# #...# #.... ..#.. #...# #...# #...# #...# #...# ....# ....#",
    "...#. #.#.. #.... #.#.# ##..# #...# #...# #...# #...# #.... ..#.. #...# #...# #...# .#.#. #...# ...#. ...#.",
    "...#. ##... #.... #.#.# #.#.# #...# ####. #...# ####. .###. ..#.. #...# #...# #.#.# ..#.. .#.#. ..#.. ..#..",
    "...#. #.#.. #.... #...# #..## #...# #.... #.#.# #.#.. ....# ..#.. #...# #...# #.#.# .#.#. ..#.. .#... .#...",
    "#..#. #..#. #.... #...# #...# #...# #.... #..#. #..#. ....# ..#.. #...# .#.#. #.#.# #...# ..#.. #.... #....",
    ".##.. #...# ##### #...# #...# .###. #.... .##.# #...# ####. ..#.. .###. ..#.. .#.#. #...# ..#.. ##### .....",
};

/* Is dot (x, y) of letter c lit?  Unknown letters (and space) are empty. */
static bool dot(char c, int x, int y)
{
    if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    const char *p = c ? strchr(font_chars, c) : NULL;
    if (!p) return false;
    int i = (int)(p - font_chars);
    return font[(i / 19) * 7 + y][(i % 19) * 6 + x] == '#';
}

static void put(uint8_t *img, unsigned w, unsigned h, int x, int y, rgb_t c)
{
    if (x < 0 || y < 0 || x >= (int)w || y >= (int)h) return;
    uint8_t *p = img + ((size_t)y * w + x) * 3;
    p[0] = c.r;
    p[1] = c.g;
    p[2] = c.b;
}

static int text_w(const char *s, int scale)
{
    size_t n = s ? strlen(s) : 0;
    return n ? (int)(n * 6 - 1) * scale : 0;
}

/* One line at height y, letters scale x scale dots (smaller if it does not
 * fit), each dot two pixels wide over a round black outline of radius ~4 */
static void write_line(uint8_t *img, unsigned w, unsigned h, int y, const char *s, int scale, align_t al, rgb_t c)
{
    static const rgb_t black = { 0, 0, 0 };
    if (!s || !*s) return;
    int tw = text_w(s, scale);
    while (scale > 1 && tw > (int)w - 8) tw = text_w(s, --scale);
    int x0 = al == ALIGN_LEFT ? 15 : al == ALIGN_RIGHT ? (int)w - tw - 15 : ((int)w - tw) / 2;
    if (x0 < 0) x0 = 0;

    for (int outline = 1; outline >= 0; --outline)
        for (size_t i = 0; s[i]; ++i)
            for (int gx = 0; gx < 5; ++gx)
                for (int gy = 0; gy < 7; ++gy) {
                    if (!dot(s[i], gx, gy)) continue;
                    for (int sy = 0; sy < scale; ++sy)
                        for (int sx = 0; sx < scale; ++sx) {
                            int px = x0 + (int)i * 6 * scale + gx * scale + sx, py = y + gy * scale + sy;
                            if (!outline) {
                                put(img, w, h, px, py, c);
                                put(img, w, h, px + 1, py, c);
                                continue;
                            }
                            for (int oy = -4; oy <= 4; ++oy)
                                for (int ox = -4; ox <= 4; ++ox)
                                    if (ox * ox + oy * oy <= 17) put(img, w, h, px + ox, py + oy, black);
                        }
                }
}

/* ---- Text colours: top (call), middle (text), bottom (our call) ---- */

#define RGB(v) { (v) >> 16, ((v) >> 8) & 0xff, (v) & 0xff }
static const rgb_t palette[20][3] = {
    /* saturated: readable over photos.  Only the first two are one colour */
    { RGB(0xff0000), RGB(0xff0000), RGB(0xff0000) },
    { RGB(0xffffff), RGB(0xffffff), RGB(0xffffff) },
    { RGB(0xffff00), RGB(0x00ff00), RGB(0x4080ff) },
    { RGB(0xff8000), RGB(0xc040ff), RGB(0x40c0ff) },
    { RGB(0xff0000), RGB(0xffff00), RGB(0x00ff00) },
    { RGB(0x4080ff), RGB(0xff8000), RGB(0xffffff) },
    { RGB(0xc040ff), RGB(0x00ff00), RGB(0xffff00) },
    { RGB(0x00ff00), RGB(0x40c0ff), RGB(0xff8000) },
    { RGB(0xffff00), RGB(0xff0000), RGB(0xc040ff) },
    { RGB(0x40c0ff), RGB(0xffff00), RGB(0xff40a0) },
    /* darker ones */
    { RGB(0x007020), RGB(0x0040a0), RGB(0xa00000) },
    { RGB(0x804000), RGB(0x600080), RGB(0x007070) },
    { RGB(0x005090), RGB(0x904000), RGB(0x700050) },
    { RGB(0x486800), RGB(0x004870), RGB(0x802020) },
    { RGB(0x700080), RGB(0x006840), RGB(0xa05000) },
    { RGB(0x005880), RGB(0x800040), RGB(0x407000) },
    { RGB(0x903000), RGB(0x304090), RGB(0x007860) },
    { RGB(0x500080), RGB(0x905000), RGB(0x006080) },
    { RGB(0x008040), RGB(0x800020), RGB(0x4040a0) },
    { RGB(0x803060), RGB(0x207050), RGB(0x906000) },
};
#define TOP    palette[colour_set][0]
#define MIDDLE palette[colour_set][1]
#define BOTTOM palette[colour_set][2]

static int scale_of(unsigned w) { return w <= 160 ? 2 : 3; }

/* CQ: the picture, "CQ CQ SSTV" top left and our call bottom right */
static void cq_picture(bool text)
{
    if (!pic_w || !pic_h) return;
    memcpy(framed, picture, (size_t)pic_w * pic_h * 3);
    if (!text) return;
    int s = scale_of(pic_w);
    write_line(framed, pic_w, pic_h, 5, "CQ CQ SSTV", s, ALIGN_LEFT, TOP);
    write_line(framed, pic_w, pic_h, (int)pic_h - 7 * s - 5, params.callsign.x, s, ALIGN_RIGHT, BOTTOM);
}

/* Middle text: a bit smaller, at most two centred lines cut at a space */
static void write_middle(const char *s)
{
    if (!s || !*s) return;
    int  scale = scale_of(pic_w), fit = ((int)pic_w - 12) / (6 * scale);
    char one[256] = { 0 }, two[256] = { 0 };
    if (fit < 4) fit = 4;
    if ((int)strlen(s) <= fit) {
        snprintf(one, sizeof(one), "%s", s);
    } else {
        int cut = fit;
        while (cut > 0 && s[cut] != ' ') cut--;
        if (!cut) cut = fit;
        snprintf(one, sizeof(one), "%.*s", cut, s);
        const char *rest = s + cut;
        while (*rest == ' ') rest++;
        snprintf(two, sizeof(two), "%.*s", fit, rest);
    }
    int lh = 7 * scale, gap = scale + 4, y = ((int)pic_h - (two[0] ? 2 * lh + gap : lh)) / 2;
    if (kind == MSG_REPLAY) y -= (int)pic_h * 34 / 256;    /* room for the small picture */
    if (y < 5) y = 5;
    write_line(framed, pic_w, pic_h, y, one, scale, ALIGN_CENTRE, MIDDLE);
    if (two[0]) write_line(framed, pic_w, pic_h, y + lh + gap, two, scale, ALIGN_CENTRE, MIDDLE);
}

/* Replay: the received picture area, ~96x72 on 320x256, 3-px black frame */
static void write_thumbnail(void)
{
    if (!rx_copy_ok) return;
    int tw = (int)pic_w * 96 / 320;
    if (tw < 48) tw = 48;
    int th = (int)((int64_t)tw * SSTV_PIC_H / SSTV_PIC_W);
    int room = pic_w <= 160 ? 24 : 38;       /* our call at the bottom */
    int x0 = (int)pic_w * 15 / 320, y0 = (int)pic_h * 135 / 256 - 3;
    if (y0 + th + 3 > (int)pic_h - room) y0 = (int)pic_h - room - th - 3;
    if (y0 < 3) y0 = 3;

    for (int y = 0; y < th; ++y) {
        int sy = (int)((int64_t)y * SSTV_PIC_H / th);
        for (int x = 0; x < tw; ++x) {
            uint32_t c = lv_color_to32(rx_copy[(size_t)sy * SSTV_PIC_W + (int)((int64_t)x * SSTV_PIC_W / tw)]);
            put(framed, pic_w, pic_h, x0 + x, y0 + y, (rgb_t){ (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff });
        }
    }
    static const rgb_t black = { 0, 0, 0 };
    for (int b = 1; b <= 3; ++b) {
        for (int x = -b; x < tw + b; ++x) {
            put(framed, pic_w, pic_h, x0 + x, y0 - b, black);
            put(framed, pic_w, pic_h, x0 + x, y0 + th - 1 + b, black);
        }
        for (int y = -b; y < th + b; ++y) {
            put(framed, pic_w, pic_h, x0 - b, y0 + y, black);
            put(framed, pic_w, pic_h, x0 + tw - 1 + b, y0 + y, black);
        }
    }
}

/* Reply / Replay: target call top left, the text, our call bottom right */
static void draw_text_picture(void)
{
    char bottom[256];
    int  s = scale_of(pic_w);

    if (!pic_w || !pic_h) return;
    memcpy(framed, picture, (size_t)pic_w * pic_h * 3);
    write_line(framed, pic_w, pic_h, 5, target[0] ? target : "<TARGET CALLSIGN>", s, ALIGN_LEFT, TOP);
    write_middle(text_middle);
    if (kind == MSG_REPLAY) write_thumbnail();
    snprintf(bottom, sizeof(bottom), "%s%s", text_bottom, params.callsign.x);
    write_line(framed, pic_w, pic_h, (int)pic_h - 7 * s - 5, bottom, s, ALIGN_RIGHT, BOTTOM);
}

/* ---- Picture area ---- */

static void rx_save(void)
{
    memcpy(rx_copy, sstv_pixels, sizeof(rx_copy));
    rx_copy_ok = true;
}

static void rx_restore(void)
{
    if (!rx_copy_ok || !sstv_canvas) return;
    memcpy(sstv_pixels, rx_copy, sizeof(rx_copy));
    lv_obj_invalidate(sstv_canvas);
}

/* framed, as large as fits, on black */
static void show_framed(void)
{
    if (!sstv_canvas || !pic_w || !pic_h) return;
    lv_canvas_fill_bg(sstv_canvas, lv_color_black(), LV_OPA_COVER);
    int ow = SSTV_PIC_W, oh = (int)((uint64_t)pic_h * ow / pic_w);
    if (oh > SSTV_PIC_H) {
        oh = SSTV_PIC_H;
        ow = (int)((uint64_t)pic_w * oh / pic_h);
    }
    int x0 = (SSTV_PIC_W - ow) / 2, y0 = (SSTV_PIC_H - oh) / 2;
    for (int y = 0; y < oh; ++y) {
        unsigned sy = (unsigned)((uint64_t)y * pic_h / oh);
        for (int x = 0; x < ow; ++x) {
            const uint8_t *p = framed + ((size_t)sy * pic_w + (unsigned)((uint64_t)x * pic_w / ow)) * 3;
            lv_canvas_set_px(sstv_canvas, x0 + x, y0 + y, lv_color_make(p[0], p[1], p[2]));
        }
    }
    lv_obj_invalidate(sstv_canvas);
}

/* src (sw x sh) into dst (dw x dh), aspect kept, centred on black, bilinear */
static void fit(const uint8_t *src, unsigned sw, unsigned sh, uint8_t *dst, unsigned dw, unsigned dh)
{
    double   k = (double)dw / sw < (double)dh / sh ? (double)dw / sw : (double)dh / sh;
    unsigned rw = (unsigned)floor(sw * k + 0.5), rh = (unsigned)floor(sh * k + 0.5);

    memset(dst, 0, (size_t)dw * dh * 3);
    rw = rw < 1 ? 1 : rw > dw ? dw : rw;
    rh = rh < 1 ? 1 : rh > dh ? dh : rh;
    unsigned x0 = (dw - rw) / 2, y0 = (dh - rh) / 2;
    for (unsigned y = 0; y < rh; ++y) {
        double sy = ((double)y + 0.5) * sh / rh - 0.5;
        if (sy < 0.0) sy = 0.0;
        unsigned y1 = (unsigned)sy, y2 = y1 + 1 < sh ? y1 + 1 : y1;
        double   fy = sy - y1;
        for (unsigned x = 0; x < rw; ++x) {
            double sx = ((double)x + 0.5) * sw / rw - 0.5;
            if (sx < 0.0) sx = 0.0;
            unsigned x1 = (unsigned)sx, x2 = x1 + 1 < sw ? x1 + 1 : x1;
            double   fx = sx - x1;
            for (unsigned c = 0; c < 3; ++c) {
                double a = src[((size_t)y1 * sw + x1) * 3 + c], b = src[((size_t)y1 * sw + x2) * 3 + c];
                double d = src[((size_t)y2 * sw + x1) * 3 + c], e = src[((size_t)y2 * sw + x2) * 3 + c];
                double top = a + (b - a) * fx, low = d + (e - d) * fx;
                dst[((size_t)(y0 + y) * dw + x0 + x) * 3 + c] = (uint8_t)(top + (low - top) * fy + 0.5);
            }
        }
    }
}

/* A picture of the TX folder at the TX mode's size (transparency on white) */
static bool picture_load(const char *file)
{
    const sstv_mode_info_t *mi = sstv_mode_info(sstv_mode);
    char                    path[512];
    unsigned                w, h;

    snprintf(path, sizeof(path), "%s/%s", SSTV_TX_PICS_DIR, file);
    uint8_t *src = sstv_png_read(path, 8192, true, &w, &h);
    if (!src) {
        fprintf(stderr, "SSTV TX: cannot read %s\n", path);
        return false;
    }
    pic_w = mi->width;
    pic_h = mi->height;
    fit(src, w, h, picture, pic_w, pic_h);
    free(src);
    cq_picture(false);
    snprintf(pic_name, sizeof(pic_name), "%s", file);
    return true;
}

/* TX Mode pressed in setup: the picture again at the new size, with its text */
void sstv_tx_mode_changed(void)
{
    if (pic_shown < 0 || (size_t)pic_shown >= pic_count || !picture_load(pic_files[pic_shown])) return;
    switch (step) {
    case STEP_MENU:         /* the menu never covers the received picture */
        rx_restore();
        return;
    case STEP_TEXT:
        draw_text_picture();
        break;
    case STEP_COLOURS:
        if (kind == MSG_CQ)
            cq_picture(true);
        else
            draw_text_picture();
        break;
    default:
        memcpy(framed, picture, (size_t)pic_w * pic_h * 3);
        break;
    }
    show_framed();
}

/* ---- On the air ---- */

static bool stop_cb(void *ctx)
{
    (void)ctx;
    return stop;
}

static void *tx_thread(void *arg)
{
    int16_t *pcm = NULL;
    size_t   n = 0;
    (void)arg;

    int rc = sstv_tx_pcm(on_air, SSTV_TX_PNG, AUDIO_PLAY_RATE, &pcm, &n);
    if (rc < 0 || stop) {
        free(pcm);
        result = rc < 0 ? rc : -1;
        running = false;
        return NULL;
    }
    const size_t pre = (size_t)AUDIO_PLAY_RATE * PREROLL_MS / 1000u;
    int16_t     *p = realloc(pcm, (pre + n) * sizeof(*pcm));
    if (!p) {
        free(pcm);
        result = -32;
        running = false;
        return NULL;
    }
    memmove(p + pre, p, n * sizeof(*p));
    memset(p, 0, pre * sizeof(*p));

    /* tx_player keys the radio and holds the power and level, as for FT8;
     * the tones are audio frequencies already, no VFO shift */
    fprintf(stderr, "SSTV TX: keying, %s\n", sstv_mode_info(on_air)->name);
    clock_gettime(CLOCK_MONOTONIC, &key_time);
    key_time_ok = true;
    keyed = true;
    bool done = tx_player_play(p, (uint32_t)(pre + n), TX_PLAYER_AUDIO_HZ, tx_player_base_gain_offset(), stop_cb, NULL);
    keyed = false;
    key_time_ok = false;
    free(p);
    if (done && !stop) progress = 1000;
    result = done && !stop ? 1 : -1;
    fprintf(stderr, "SSTV TX: %s\n", result == 1 ? "done" : "stopped");
    running = false;
    return NULL;
}

/* Back to listening after any transmission, sent, stopped or failed */
static void on_air_end(void)
{
    sstv_tx_state = SSTV_TX_OFF;
    step = STEP_NONE;
    sstv_btn_tx.label = "TX";
    sstv_mode_button(false, sstv_mode);
    sstv_rx_back(true);
    rx_restore();
    if (sstv_btn_tx.disp_btn) buttons_refresh(&sstv_btn_tx);
    sstv_controls();
}

static bool on_air_start(sstv_mode_t mode)
{
    char s[96];

    if (!pic_w || !pic_h) {
        sstv_status("SSTV TX: no final image.");
        return false;
    }
    /* framed is already at the mode's size: the encoder takes it as it is */
    if (!sstv_png_write(SSTV_TX_PNG, framed, pic_w, pic_h)) {
        sstv_status("SSTV TX: cannot prepare image.");
        return false;
    }
    if (thread_on) {
        pthread_join(thread, NULL);
        thread_on = false;
    }
    on_air = mode;
    stop = keyed = key_time_ok = false;
    progress = 0;
    result = 0;
    running = true;

    sstv_tx_state = SSTV_TX_ON_AIR;
    step = STEP_NONE;
    sstv_btn_tx.label = "Halt TX";
    sstv_mode_button(true, on_air);
    status_text_show(true);
    snprintf(s, sizeof(s), "Preparing %s TX...", sstv_mode_info(on_air)->label);
    sstv_status(s);
    if (sstv_bar) {
        lv_bar_set_value(sstv_bar, 0, LV_ANIM_OFF);
        lv_obj_clear_flag(sstv_bar, LV_OBJ_FLAG_HIDDEN);
    }
    if (sstv_btn_tx.disp_btn) buttons_refresh(&sstv_btn_tx);
    sstv_controls();

    int rc = pthread_create(&thread, NULL, tx_thread, NULL);
    if (rc) {
        fprintf(stderr, "SSTV TX: cannot start the TX thread: %d\n", rc);
        running = false;
        result = -31;
        sstv_status("SSTV TX: thread creation failed.");
        if (sstv_bar) lv_obj_add_flag(sstv_bar, LV_OBJ_FLAG_HIDDEN);
        on_air_end();
        return false;
    }
    thread_on = true;
    return true;
}

/* ---- Menu ---- */

static void focus_picture_back(void)
{
    sstv_focus_picture();
    status_text_show(true);
}

static void pictures_close(void);

static void image_only_go(void)
{
    memcpy(framed, picture, (size_t)pic_w * pic_h * 3);
    show_framed();
    pictures_close();
    box_close();
    focus_picture_back();
    on_air_start(sstv_mode);
}

static void menu_close(void) { list_drop(&menu); }

static void menu_look_cb(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *d = lv_event_get_draw_part_dsc(e);
    if (d && d->part == LV_PART_ITEMS && d->label_dsc &&
        (((d->id == 1 || d->id == 2) && !target[0]) || (d->id == 4 && !last_ok)))
        d->label_dsc->color = TC(text_mute);
}

static void menu_hint(void)
{
    static const char *const hints[] = {
        "A picture, then CQ CQ SSTV and your callsign on it",
        "A picture and a text template, with the target callsign",
        "As Reply, with a miniature of the last received picture",
        "A picture, sent without any text",
        "The last transmission again, same picture and mode",
        "It goes in the text of Reply and Replay",
    };
    if (!menu || !sstv_frame.status) return;
    int         r = list_row(menu);
    const char *h = r >= 0 && r <= 5 ? hints[r] : HINT;
    if ((r == 1 || r == 2) && !target[0]) h = "Set the target callsign first";
    if (r == 4 && !last_ok) h = "No transmission to repeat yet";
    lv_label_set_text(sstv_frame.status, h);
}

static void menu_moved_cb(lv_event_t *e)
{
    (void)e;
    int r = list_row(menu);
    if (r >= 0 && r <= 5) menu_row = r;
    menu_hint();
}

static bool kbd_ok_cb(void)
{
    const char *v = textarea_window_get();
    if (v) {
        while (*v == ' ' || *v == '\t') v++;
        snprintf(target, sizeof(target), "%s", v);
        size_t n = strlen(target);
        while (n && (target[n - 1] == ' ' || target[n - 1] == '\t')) target[--n] = 0;
        for (char *c = target; *c; ++c)
            if (*c >= 'a' && *c <= 'z') *c -= 'a' - 'A';
    }
    textarea_window_close();
    menu_open();
    return true;
}

static bool kbd_cancel_cb(void)
{
    textarea_window_close();
    menu_open();
    return true;
}

static lv_obj_t *find_keyboard(lv_obj_t *o)
{
    if (!o) return NULL;
    if (lv_obj_check_type(o, &lv_keyboard_class)) return o;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(o); ++i) {
        lv_obj_t *k = find_keyboard(lv_obj_get_child(o, i));
        if (k) return k;
    }
    return NULL;
}

/* lower case keys: calls are typed in capitals anyway, and some key events
 * that arrive just after the window opens switch its page */
static void kbd_lower(void)
{
    if (!keys) keys = find_keyboard(lv_scr_act());
    if (keys && lv_obj_is_valid(keys))
        lv_keyboard_set_mode(keys, LV_KEYBOARD_MODE_TEXT_LOWER);
    else
        keys = NULL;
}

static void kbd_show(void)
{
    textarea_window_open(kbd_ok_cb, kbd_cancel_cb);
    keys = find_keyboard(lv_scr_act());
    kbd_lower_until = lv_tick_get() + 500;
    kbd_lower();
    lv_obj_t *t = textarea_window_text();
    lv_textarea_set_accepted_chars(t, "0123456789/ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
    lv_textarea_set_max_length(t, sizeof(target) - 1);
    lv_textarea_set_placeholder_text(t, "Target callsign");
    textarea_window_set(target);
}

static void menu_pressed_cb(lv_event_t *e)
{
    (void)e;
    if (sstv_tx_state != SSTV_TX_SETUP || step != STEP_MENU || !menu) return;
    int r = list_row(menu);
    switch (r) {
    case 1:
    case 2:
        if (!target[0]) return;    /* Reply and Replay need a target */
        /* fall through */
    case 0:
    case 3:
        kind = r == 0 ? MSG_CQ : r == 1 ? MSG_REPLY : r == 2 ? MSG_REPLAY : MSG_IMAGE;
        menu_close();
        step = STEP_PICTURE;
        pictures_open();
        return;
    case 4:
        if (!last_ok) return;
        kind = MSG_REPEAT;
        menu_close();
        box_close();
        pic_w = last_w;
        pic_h = last_h;
        memcpy(framed, last, (size_t)last_w * last_h * 3);
        show_framed();
        focus_picture_back();
        on_air_start(last_mode);
        return;
    case 5:
        lv_group_remove_obj(menu);
        kbd_open = true;           /* from the UI timer, as the CALLSIGN window */
        return;
    default:
        return;
    }
}

static void menu_open(void)
{
    char reply[64], replay[64];

    menu_close();
    step = STEP_MENU;
    status_text_show(false);
    rx_restore();                  /* the received picture stays in view */
    box_open("SSTV TX", HINT);
    menu = list_new(6);
    snprintf(reply, sizeof(reply), target[0] ? "REPLY TO %s" : "REPLY TO...", target);
    snprintf(replay, sizeof(replay), target[0] ? "REPLAY TO %s" : "REPLAY TO...", target);
    lv_table_set_cell_value(menu, 0, 0, "CALL CQ");
    lv_table_set_cell_value(menu, 1, 0, reply);
    lv_table_set_cell_value(menu, 2, 0, replay);
    lv_table_set_cell_value(menu, 3, 0, "TX IMAGE ONLY");
    lv_table_set_cell_value(menu, 4, 0, "REPEAT LAST TX");
    lv_table_set_cell_value(menu, 5, 0, "SET TARGET CALLSIGN");
    lv_obj_add_event_cb(menu, menu_look_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    lv_obj_add_event_cb(menu, menu_moved_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(menu, menu_pressed_cb, LV_EVENT_PRESSED, NULL);
    list_focus(menu);

    /* the row used last; Reply/Replay with no target: Set target instead */
    int r = menu_row >= 0 && menu_row <= 5 ? menu_row : 0;
    if ((r == 1 || r == 2) && !target[0]) r = 5;
    if (r == 4 && !last_ok) r = 0;
    list_go(menu, r);
    menu_row = r;
    menu_hint();
}

/* ---- Pictures ---- */

static void pictures_close(void)
{
    list_drop(&pictures);
    files_free(&pic_files, &pic_count);
    pic_shown = pic_wanted = -1;
    pic_wanted_at = 0;
    pic_name[0] = 0;
}

static void picture_show_wanted(void)
{
    if (pic_wanted < 0 || pic_wanted == pic_shown || (size_t)pic_wanted >= pic_count) return;
    if (ms_now() - pic_wanted_at < SETTLE_MS) return;
    int r = pic_wanted;
    if (picture_load(pic_files[r])) {
        pic_shown = r;
        show_framed();
    }
}

static void pictures_moved_cb(lv_event_t *e)
{
    (void)e;
    if (sstv_tx_state != SSTV_TX_SETUP || step != STEP_PICTURE || !pictures || !pic_count) return;
    int r = list_row(pictures);
    if (r < 0 || (size_t)r >= pic_count) return;
    pic_wanted = r;
    pic_wanted_at = ms_now();
}

static void pictures_pressed_cb(lv_event_t *e)
{
    (void)e;
    if (sstv_tx_state != SSTV_TX_SETUP || step != STEP_PICTURE || !pic_count) return;
    int r = list_row(pictures);
    if (r < 0 || (size_t)r >= pic_count) return;

    /* a quick turn and press confirms the row shown, loaded or not yet */
    pic_wanted = r;
    if (pic_shown != r) {
        if (!picture_load(pic_files[r])) return;
        pic_shown = r;
        show_framed();
    }
    snprintf(last_file, sizeof(last_file), "%s", pic_files[r]);
    lv_group_remove_obj(pictures);
    lv_obj_add_flag(pictures, LV_OBJ_FLAG_HIDDEN);

    if (kind == MSG_IMAGE) {
        image_only_go();
    } else if (kind == MSG_CQ) {
        cq_picture(true);
        show_framed();
        step = STEP_COLOURS;
        colours_open();
    } else {
        step = STEP_TEXT;
        texts_open();
    }
}

static void pictures_open(void)
{
    pictures_close();
    files_scan(SSTV_TX_PICS_DIR, ".png", false, &pic_files, &pic_count);
    status_text_show(false);
    box_open("TX PICTURE", "MFK: choose - press: confirm - TX Mode: mode - Cancel TX: back");
    pictures = list_new(1);

    if (!pic_count) {
        lv_table_set_cell_value(pictures, 0, 0, "No TX pictures found");
        buttons_disabled(&sstv_btn_mode, false);
        sstv_focus_picture();
        sstv_status("No TX pictures found");
        if (sstv_frame.status) lv_label_set_text(sstv_frame.status, "Put PNG pictures in " SSTV_TX_PICS_DIR);
        return;
    }
    lv_table_set_row_cnt(pictures, pic_count);
    for (size_t i = 0; i < pic_count; ++i) {
        /* one line each: name without .png, cut with "..." past 22 letters */
        char   name[64];
        size_t n = strlen(pic_files[i]) - 4;
        snprintf(name, sizeof(name), n <= 22 ? "%.*s" : "%.*s...", (int)(n <= 22 ? n : 19), pic_files[i]);
        lv_table_set_cell_value(pictures, i, 0, name);
    }
    lv_obj_add_event_cb(pictures, pictures_moved_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(pictures, pictures_pressed_cb, LV_EVENT_PRESSED, NULL);
    list_focus(pictures);

    int r = 0;
    for (size_t i = 0; last_file[0] && i < pic_count; ++i)
        if (!strcmp(pic_files[i], last_file)) {
            r = (int)i;
            break;
        }
    list_go(pictures, r);
    pic_wanted = r;
    pic_wanted_at = ms_now() - SETTLE_MS;
    picture_show_wanted();
}

/* ---- Texts (*.sstvtxt: JSON with "middle" and "bottom") ---- */

/* The string value of key in json, \n and \t as spaces; a missing key or
 * null is an empty string.  false if the value is not a string. */
static bool json_text(const char *json, const char *key, char *out, size_t size)
{
    char        k[64];
    const char *p;
    size_t      n = 0;

    out[0] = 0;
    snprintf(k, sizeof(k), "\"%s\"", key);
    if (!(p = strstr(json, k))) return true;
    if (!(p = strchr(p + strlen(k), ':'))) return false;
    for (p++; *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'; p++) {}
    if (!strncmp(p, "null", 4)) return true;
    if (*p++ != '"') return false;
    for (; *p && *p != '"'; ++p) {
        char c = *p;
        if (c == '\\' && p[1]) {
            c = *++p;
            if (c == 'n' || c == 't') c = ' ';
        }
        if (n + 1 < size) out[n++] = c;
    }
    if (*p != '"') return false;
    out[n] = 0;
    return true;
}

static bool text_load(const char *file)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", SSTV_TX_TEXT_DIR, file);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    long size = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
    char *json = size >= 0 && size <= 16384 ? malloc((size_t)size + 1) : NULL;
    if (!json) {
        fclose(f);
        return false;
    }
    rewind(f);
    json[fread(json, 1, (size_t)size, f)] = 0;
    fclose(f);
    bool ok = json_text(json, "middle", text_middle, sizeof(text_middle)) &&
              json_text(json, "bottom", text_bottom, sizeof(text_bottom));
    free(json);
    return ok;
}

static void text_name(const char *file, char *out, size_t size)
{
    size_t n = strlen(file);
    snprintf(out, size, "%.*s", (int)(n >= 8 ? n - 8 : n), file);   /* without .sstvtxt */
}

static void texts_close(void)
{
    text_redraw = false;
    list_drop(&texts);
    files_free(&text_files, &text_count);
}

static void text_show_selected(void)
{
    if (!texts || !text_count) return;
    int r = list_row(texts);
    if (r < 0 || (size_t)r >= text_count) return;
    if (!text_load(text_files[r])) {
        fprintf(stderr, "SSTV TX: %s is not a valid text\n", text_files[r]);
        return;
    }
    draw_text_picture();
    show_framed();
}

static void texts_moved_cb(lv_event_t *e)
{
    (void)e;
    if (sstv_tx_state == SSTV_TX_SETUP && step == STEP_TEXT) text_redraw = true;
}

static void texts_pressed_cb(lv_event_t *e)
{
    (void)e;
    if (sstv_tx_state != SSTV_TX_SETUP || step != STEP_TEXT || !text_count) return;
    text_redraw = false;
    text_show_selected();
    int r = list_row(texts);
    if (r < 0 || (size_t)r >= text_count) return;
    texts_close();
    if (kind != MSG_REPLAY) kind = MSG_REPLY;
    step = STEP_COLOURS;
    colours_open();
}

static void texts_open(void)
{
    texts_close();
    files_scan(SSTV_TX_TEXT_DIR, ".sstvtxt", true, &text_files, &text_count);
    status_text_show(false);
    box_open("TX TEXT", HINT);
    texts = list_new(1);
    if (!text_count) {
        lv_table_set_cell_value(texts, 0, 0, "No TX templates found");
        sstv_focus_picture();
        return;
    }
    lv_table_set_row_cnt(texts, (uint16_t)text_count);
    for (size_t i = 0; i < text_count; ++i) {
        char name[256];
        text_name(text_files[i], name, sizeof(name));
        lv_table_set_cell_value(texts, (uint16_t)i, 0, name);
    }
    lv_obj_add_event_cb(texts, texts_moved_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(texts, texts_pressed_cb, LV_EVENT_PRESSED, NULL);
    list_focus(texts);
    text_redraw = true;
}

/* ---- Colours ---- */

static void colour_use(int r)
{
    colour_set = colour_row = r >= 0 && r < 20 ? r : 0;
}

static void colour_show_selected(void)
{
    if (!colours) return;
    int r = list_row(colours);
    if (r < 0 || r >= 20) return;
    colour_use(r);
    if (kind == MSG_CQ)
        cq_picture(true);
    else
        draw_text_picture();
    show_framed();
}

static void colours_close(void)
{
    colour_redraw = false;
    colour_moved_at = 0;
    list_drop(&colours);
}

static void colours_moved_cb(lv_event_t *e)
{
    (void)e;
    if (sstv_tx_state == SSTV_TX_SETUP && step == STEP_COLOURS) {
        colour_redraw = true;
        colour_moved_at = lv_tick_get();
    }
}

static void colours_pressed_cb(lv_event_t *e)
{
    (void)e;
    if (sstv_tx_state != SSTV_TX_SETUP || step != STEP_COLOURS || !colours) return;
    colour_redraw = false;          /* the row shown, drawn or not yet */
    colour_moved_at = 0;
    colour_show_selected();
    colours_close();
    box_close();
    focus_picture_back();
    on_air_start(sstv_mode);       /* CQ, Reply and Replay all go out from here */
}

/* three swatches in the (blank) row */
static void swatch_cb(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *d = lv_event_get_draw_part_dsc(e);
    if (!d || d->part != LV_PART_ITEMS || d->id >= 20 || !d->draw_area) return;

    const int   size = 22, gap = 10;
    lv_area_t   a = *d->draw_area;
    int         x = a.x1 + (lv_area_get_width(&a) - (3 * size + 2 * gap)) / 2;
    int         y = a.y1 + (lv_area_get_height(&a) - size) / 2;
    for (int i = 0; i < 3; ++i) {
        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.bg_opa = LV_OPA_COVER;
        r.bg_color = lv_color_make(palette[d->id][i].r, palette[d->id][i].g, palette[d->id][i].b);
        r.border_width = 2;
        r.border_opa = LV_OPA_COVER;
        r.border_color = lv_color_black();
        lv_area_t at = { x + i * (size + gap), y, x + i * (size + gap) + size - 1, y + size - 1 };
        lv_draw_rect(d->draw_ctx, &r, &at);
    }
}

static void colours_open(void)
{
    colours_close();
    status_text_show(false);
    box_open("TEXT COLORS", "MFK: choose - press: start the transmission - Cancel TX: back");
    colours = list_new(20);        /* styled before the rows are filled: LVGL sizes them then */
    for (int i = 0; i < 20; ++i) lv_table_set_cell_value(colours, i, 0, " ");
    lv_obj_add_event_cb(colours, swatch_cb, LV_EVENT_DRAW_PART_END, NULL);
    lv_obj_add_event_cb(colours, colours_moved_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(colours, colours_pressed_cb, LV_EVENT_PRESSED, NULL);
    list_focus(colours);

    int r = colour_row >= 0 && colour_row < 20 ? colour_row : 0;
    list_go(colours, r);
    colour_use(r);
    colour_redraw = true;
    colour_moved_at = lv_tick_get() - 300;
}

/* ---- From app_sstv.c ---- */

static void setup_close(void)
{
    colours_close();
    texts_close();
    menu_close();
    pictures_close();
    box_close();
    step = STEP_NONE;
}

void sstv_tx_button(void)
{
    if (sstv_tx_state == SSTV_TX_ON_AIR) {
        stop = true;
        sstv_status("Stopping SSTV TX...");
        return;
    }
    if (sstv_tx_state == SSTV_TX_SETUP) {     /* Cancel TX */
        setup_close();
        status_text_show(true);
        sstv_tx_state = SSTV_TX_OFF;
        sstv_focus_picture();     /* the lists had the focus: volume keys back to the picture */
        sstv_btn_tx.label = "TX";
        sstv_mode_button(false, sstv_mode);
        sstv_rx_back(auto_after);
        rx_restore();
        if (sstv_btn_tx.disp_btn) buttons_refresh(&sstv_btn_tx);
        sstv_controls();
        return;
    }

    rx_save();                    /* before any TX picture covers it */
    auto_after = sstv_rx_stop_for_tx();
    sstv_tx_state = SSTV_TX_SETUP;
    step = STEP_MENU;
    sstv_btn_tx.label = "Cancel TX";
    sstv_mode_button(true, sstv_mode);
    sstv_status("TX setup\n\nChoose TX action");
    if (sstv_bar) {
        lv_bar_set_value(sstv_bar, 0, LV_ANIM_OFF);
        lv_obj_add_flag(sstv_bar, LV_OBJ_FLAG_HIDDEN);
    }
    if (sstv_btn_tx.disp_btn) buttons_refresh(&sstv_btn_tx);
    sstv_controls();
    menu_open();
}

void sstv_tx_halt(void)
{
    stop = true;
}

bool sstv_tx_busy(void) { return running; }
int sstv_tx_progress(void) { return progress; }
sstv_mode_t sstv_tx_mode(void) { return on_air; }

void sstv_tx_tick(void)
{
    if (kbd_open) {
        kbd_open = false;
        kbd_show();
    }
    if (kbd_lower_until) {
        kbd_lower();
        if ((int32_t)(kbd_lower_until - lv_tick_get()) <= 0) kbd_lower_until = 0;
    }
    if (sstv_tx_state == SSTV_TX_SETUP) {
        if (step == STEP_PICTURE) picture_show_wanted();
        if (step == STEP_TEXT && text_redraw) {
            text_redraw = false;
            text_show_selected();
        }
        if (step == STEP_COLOURS && colour_redraw && (uint32_t)(lv_tick_get() - colour_moved_at) >= 300) {
            colour_redraw = false;
            colour_show_selected();
        }
    }

    if (running) {
        if (sstv_bar) {
            int v = 0;
            if (key_time_ok) {
                uint64_t t = ms_since(&key_time);
                unsigned len = sstv_mode_info(on_air)->tx_ms;
                if (t > PREROLL_MS && len) v = t - PREROLL_MS >= len ? 1000 : (int)((t - PREROLL_MS) * 1000u / len);
            }
            progress = v;
            app_frame_bar(&sstv_frame, v, theme_hex(TC_tx));     /* red while transmitting */
        }
        if (keyed) {
            char s[96];
            snprintf(s, sizeof(s), "Transmitting %s...", sstv_mode_info(on_air)->label);
            sstv_status(s);
        }
        return;
    }
    if (!thread_on) return;

    char s[96];
    pthread_join(thread, NULL);
    thread_on = false;
    if (sstv_bar) {
        lv_bar_set_value(sstv_bar, result == 1 ? 1000 : 0, LV_ANIM_OFF);
        lv_obj_add_flag(sstv_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(sstv_bar, TC(ok_bar), LV_PART_INDICATOR);
    }
    if (result == 1) {
        /* only a complete transmission becomes the one to repeat */
        if (pic_w && pic_h && (size_t)pic_w * pic_h * 3 <= sizeof(last)) {
            memcpy(last, framed, (size_t)pic_w * pic_h * 3);
            last_w = pic_w;
            last_h = pic_h;
            last_mode = on_air;
            last_ok = true;
        }
        snprintf(s, sizeof(s), "SSTV %s TX complete", sstv_mode_info(on_air)->label);
        msg_schedule_text_fmt("%s", s);
    } else if (result == -1) {
        msg_schedule_text_fmt("SSTV TX halted");
    } else {
        snprintf(s, sizeof(s), "SSTV TX failed (%d)", result);
        msg_schedule_text_fmt("%s", s);
    }
    keyed = false;
    progress = 0;
    on_air_end();                 /* sent, halted or failed: listening again for the answer */
}

void sstv_tx_close(void)
{
    if (thread_on) {
        stop = true;
        pthread_join(thread, NULL);
        thread_on = false;
    }
    running = keyed = false;
    sstv_btn_tx.label = "TX";
    sstv_tx_state = SSTV_TX_OFF;
    auto_after = false;
    setup_close();
}
