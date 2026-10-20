/*
 * SSTV app: the received pictures in /mnt/sstv, newest first.  A list over
 * the whole body; the MFK press shows a picture as large as it fits, Done
 * (or the MFK) goes back to the list.  Receiving goes on underneath.
 */
#include "app_sstv_int.h"

#include "keyboard.h"
#include "sstv/sstv_png.h"
#include "styles.h"
#include "theme.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VIEW_W 710
#define VIEW_H (theme_shape.vision ? SSTV_PIC_H : APP_FRAME_BODY_H - 4)    /* VISION: the window's height */
#define VIEW_Y (theme_shape.vision ? SSTV_PIC_Y : APP_FRAME_BODY_Y + 2)
#define VIEW_H_MAX (DIALOG_H_MAX - 20)                                     /* view_px, kept across themes */

static lv_obj_t   *back, *list, *view;
static lv_color_t *view_px;
static char      **files;
static unsigned    count;
static bool        open_now, viewing;
static uint8_t    *img;
static unsigned    img_w, img_h;

static void files_free(void)
{
    for (unsigned i = 0; i < count; ++i) free(files[i]);
    free(files);
    files = NULL;
    count = 0;
}

static int newest_first(const void *a, const void *b)
{
    return strcmp(*(char *const *)b, *(char *const *)a);    /* the names carry the time */
}

static void scan(void)
{
    DIR           *d = opendir(SSTV_DIR);
    struct dirent *e;
    size_t         room = 0;

    files_free();
    if (!d) return;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".png") || strncmp(e->d_name, "sstv-", 5)) continue;
        if (count == room) {
            char **p = realloc(files, (room ? room * 2 : 128) * sizeof(char *));
            if (!p) break;
            files = p;
            room = room ? room * 2 : 128;
        }
        if ((files[count] = strdup(e->d_name))) ++count;
    }
    closedir(d);
    qsort(files, count, sizeof(char *), newest_first);
}

static void list_fill(void)
{
    if (!list) return;
    if (!count) {
        lv_table_set_row_cnt(list, 1);
        lv_table_set_cell_value(list, 0, 0, "No saved pictures");
    } else {
        lv_table_set_row_cnt(list, count);
        for (unsigned i = 0; i < count; ++i) lv_table_set_cell_value(list, i, 0, files[i]);
    }
    lv_obj_invalidate(list);
}

void sstv_gallery_labels(void)
{
    sstv_btn_delete.label = viewing ? "Done" : "Delete\npicture";
    buttons_disabled(&sstv_btn_delete, !viewing && (!open_now || !count));
    sstv_btn_gallery.label = open_now ? "Close\nBrowser" : "RX Pics\nBrowser";
    if (sstv_btn_gallery.disp_btn) buttons_refresh(&sstv_btn_gallery);
    if (sstv_btn_delete.disp_btn) buttons_refresh(&sstv_btn_delete);
}

static void img_free(void)
{
    free(img);
    img = NULL;
    img_w = img_h = 0;
    viewing = false;
}

static void focus(lv_obj_t *o)
{
    lv_group_add_obj(keyboard_group, o);
    lv_group_focus_obj(o);
    lv_group_set_editing(keyboard_group, true);
}

static void to_list(void)
{
    if (!open_now) return;
    if (view) {
        lv_group_remove_obj(view);
        lv_obj_add_flag(view, LV_OBJ_FLAG_HIDDEN);
    }
    img_free();
    scan();
    if (list) {
        lv_obj_clear_flag(list, LV_OBJ_FLAG_HIDDEN);
        list_fill();
        focus(list);
        lv_obj_invalidate(list);
    }
    sstv_gallery_labels();
}

static void view_pressed_cb(lv_event_t *e)
{
    (void)e;
    to_list();
}

/* the picture as large as fits on black, nearest pixel */
static void view_draw(void)
{
    if (!view_px || !img) return;
    unsigned ow = VIEW_W, oh = (unsigned)((uint64_t)img_h * VIEW_W / img_w);
    if (oh > VIEW_H) {
        oh = VIEW_H;
        ow = (unsigned)((uint64_t)img_w * VIEW_H / img_h);
    }
    if (ow < 1) ow = 1;
    if (oh < 1) oh = 1;
    unsigned x0 = (VIEW_W - ow) / 2, y0 = (VIEW_H - oh) / 2;

    for (unsigned i = 0; i < VIEW_W * VIEW_H; ++i) view_px[i] = lv_color_make(0, 0, 0);
    for (unsigned y = 0; y < oh; ++y) {
        unsigned sy = (unsigned)((uint64_t)y * img_h / oh);
        if (sy >= img_h) sy = img_h - 1;
        for (unsigned x = 0; x < ow; ++x) {
            unsigned sx = (unsigned)((uint64_t)x * img_w / ow);
            if (sx >= img_w) sx = img_w - 1;
            const uint8_t *p = img + ((size_t)sy * img_w + sx) * 3;
            view_px[(y0 + y) * VIEW_W + x0 + x] = lv_color_make(p[0], p[1], p[2]);
        }
    }
    if (view) lv_obj_invalidate(view);
}

static bool view_open(const char *name)
{
    char     path[256];
    unsigned w, h;

    snprintf(path, sizeof(path), "%s/%s", SSTV_DIR, name);
    uint8_t *p = sstv_png_read(path, 10000, false, &w, &h);
    if (!p) {
        fprintf(stderr, "SSTV: cannot show %s\n", path);
        return false;
    }
    img_free();
    img = p;
    img_w = w;
    img_h = h;
    if (!view_px && !(view_px = malloc((size_t)VIEW_W * VIEW_H_MAX * sizeof(lv_color_t)))) {
        img_free();
        return false;
    }
    if (!view) {
        view = lv_canvas_create(dialog_sstv->obj);
        lv_canvas_set_buffer(view, view_px, VIEW_W, VIEW_H, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_size(view, VIEW_W, VIEW_H);
        lv_obj_set_pos(view, APP_FRAME_X + (APP_FRAME_W - VIEW_W) / 2, VIEW_Y);
        lv_obj_add_event_cb(view, view_pressed_cb, LV_EVENT_PRESSED, NULL);   /* MFK: back to the list */
    }
    view_draw();
    if (list) {
        lv_group_remove_obj(list);
        lv_obj_add_flag(list, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(view, LV_OBJ_FLAG_HIDDEN);
    focus(view);
    viewing = true;
    sstv_gallery_labels();
    return true;
}

static int selected(void)
{
    uint16_t row = LV_TABLE_CELL_NONE, col = LV_TABLE_CELL_NONE;
    if (!list || !count) return -1;
    lv_table_get_selected_cell(list, &row, &col);
    return row == LV_TABLE_CELL_NONE || row >= count ? -1 : row;
}

static void list_pressed_cb(lv_event_t *e)
{
    (void)e;
    int r = selected();
    if (r >= 0) view_open(files[r]);
}

static void delete_selected(void)
{
    char path[256];
    int  r = open_now && !viewing ? selected() : -1;

    if (r < 0) return;
    snprintf(path, sizeof(path), "%s/%s", SSTV_DIR, files[r]);
    if (unlink(path)) {
        fprintf(stderr, "SSTV: cannot delete %s: %s\n", path, strerror(errno));
        return;
    }
    scan();                     /* the last one gone: "No saved pictures" at once */
    list_fill();
    sstv_gallery_labels();
    lv_group_focus_obj(list);
    lv_group_set_editing(keyboard_group, true);
}

static void gallery_show(void)
{
    if (open_now) return;
    scan();

    /* opaque body over the picture and the texts */
    back = lv_obj_create(dialog_sstv->obj);
    lv_obj_remove_style_all(back);
    app_frame_body(back);
    if (theme_shape.vision) {   /* over the column at the picture's right too (app_sstv.c) */
        lv_obj_set_pos(back, APP_FRAME_X, 8);
        lv_obj_set_height(back, DIALOG_H - 16);
    }
    lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(back, LV_OBJ_FLAG_CLICKABLE);

    list = lv_table_create(dialog_sstv->obj);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_size(list, 700, theme_shape.vision ? DIALOG_H - 32 : APP_FRAME_BODY_H - 16);
    lv_obj_set_pos(list, APP_FRAME_X + (APP_FRAME_W - 700) / 2, theme_shape.vision ? 16 : APP_FRAME_BODY_Y + 8);
    lv_table_set_col_cnt(list, 1);
    lv_table_set_col_width(list, 0, 690);
    lv_obj_set_style_bg_color(list, TC(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_text_color(list, TC(text), LV_PART_ITEMS);
    lv_obj_set_style_text_font(list, &sony_24, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(list, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(list, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(list, 12, LV_PART_ITEMS);
    lv_obj_set_style_text_color(list, TC(text_mute), LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_EDITED);
    list_fill();
    lv_obj_add_event_cb(list, list_pressed_cb, LV_EVENT_PRESSED, NULL);
    focus(list);

    open_now = true;
    viewing = false;
    sstv_gallery_labels();
}

void sstv_gallery_close(void)
{
    if (list) {
        lv_group_remove_obj(list);
        lv_obj_del(list);
        list = NULL;
    }
    if (view) {
        lv_group_remove_obj(view);
        lv_obj_del(view);
        view = NULL;
    }
    if (back) {
        lv_obj_del(back);
        back = NULL;
    }
    free(view_px);
    view_px = NULL;
    img_free();
    files_free();
    open_now = viewing = false;
    if (dialog_sstv->obj) lv_obj_invalidate(dialog_sstv->obj);
    sstv_gallery_labels();
}

void sstv_gallery_button(void)
{
    if (open_now)
        sstv_gallery_close();
    else
        gallery_show();
}

void sstv_gallery_second(void)
{
    if (viewing)
        to_list();
    else
        delete_selected();
}

bool sstv_gallery_open(void) { return open_now; }
