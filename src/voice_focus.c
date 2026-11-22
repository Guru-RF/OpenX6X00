/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - speaks what the knob lands on (accessibility)
 *
 *  No window of this GUI announces its controls itself, so this listens to the keyboard group, the
 *  knob's and the keys' focus: whatever gets the focus is described and spoken, with the name of
 *  its row when it sits in a grid (Settings and the like), and its value is spoken again as it is
 *  changed (a switch flipped, a slider or a dropdown turned). From Jimr9's OpenX6X00 Speech
 *  Access and his X6100 Accessibility fork (github.com/Jimr9/X6100-Accessibility-Testing).
 */

#include "voice_focus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "voice.h"

static void append(char *buf, size_t size, const char *txt) {
    if (!txt || !txt[0] || size < 4) {
        return;
    }
    size_t len = strlen(buf);

    if (len > 0 && len + 2 < size) {
        buf[len++] = ',';
        buf[len++] = ' ';
        buf[len] = '\0';
    }
    if (len + 1 < size) {
        strncat(buf, txt, size - len - 1);
    }
}

/* The text of the labels inside obj (a button's caption, a list entry's icon and text), in order */
static void collect_labels(lv_obj_t *obj, char *buf, size_t size, int depth) {
    uint32_t n = lv_obj_get_child_cnt(obj);

    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(obj, i);

        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        if (lv_obj_check_type(c, &lv_label_class)) {
            append(buf, size, lv_label_get_text(c));
        } else if (depth < 2) {
            collect_labels(c, buf, size, depth + 1);
        }
    }
}

/* All of a window's text, deeper than collect_labels(), tables' cells too */
static void collect_all(lv_obj_t *obj, char *buf, size_t size, int depth) {
    uint32_t n = lv_obj_get_child_cnt(obj);

    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(obj, i);

        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        if (lv_obj_check_type(c, &lv_label_class)) {
            append(buf, size, lv_label_get_text(c));
        } else if (lv_obj_check_type(c, &lv_table_class)) {
            for (uint16_t r = 0; r < lv_table_get_row_cnt(c) && r < 12; r++) {
                for (uint16_t k = 0; k < lv_table_get_col_cnt(c); k++) {
                    append(buf, size, lv_table_get_cell_value(c, r, k));
                }
            }
        } else if (depth < 6) {
            collect_all(c, buf, size, depth + 1);
        }
    }
}

void voice_focus_read(lv_obj_t *obj) {
    voice_focus_read_as(NULL, obj);
}

void voice_focus_read_as(const char *head, lv_obj_t *obj) {
    voice_focus_read_full(head, obj, NULL);
}

void voice_focus_read_full(const char *head, lv_obj_t *obj, const char *tail) {
    char buf[1000];

    if (!obj || !voice_auto_active()) {
        return;
    }
    buf[0] = '\0';
    lv_obj_update_layout(obj);      /* the labels' real text, not a "..." cut made before the layout */
    append(buf, sizeof(buf), head);
    collect_all(obj, buf, sizeof(buf), 0);
    if (tail && tail[0]) {
        size_t n = strlen(buf);

        if (n > sizeof(buf) - 300) n = sizeof(buf) - 300;   /* room for the keys after a long screen */
        snprintf(buf + n, sizeof(buf) - n, ". %s", tail);
    }
    if (buf[0]) {
        voice_say_clean(buf, false);
    } else {
        voice_say_text_fmt("Nothing to read");
    }
}

/* ---- The line reader (voice_lines_set) ---- */

#define LINES_MAX  48
#define LINE_LEN   200
#define PIECES_MAX 96

typedef struct {
    lv_coord_t x, y;
    const char *text;
} piece_t;

static char lines[LINES_MAX][LINE_LEN];
static int  lines_n, line_at;

static void gather(lv_obj_t *obj, piece_t *p, int *n, int depth) {
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(obj) && *n < PIECES_MAX; i++) {
        lv_obj_t *c = lv_obj_get_child(obj, i);

        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
        if (lv_obj_check_type(c, &lv_label_class)) {
            const char *t = lv_label_get_text(c);
            lv_area_t   a;

            if (!t || !t[0]) continue;
            lv_obj_get_coords(c, &a);
            p[*n].x    = a.x1;
            p[*n].y    = a.y1;
            p[*n].text = t;
            (*n)++;
        } else if (depth < 6) {
            gather(c, p, n, depth + 1);
        }
    }
}

/* Column by column (a column: labels starting within 8 px of each other), top to bottom in each */
static int by_place(const void *a, const void *b) {
    const piece_t *p = a, *q = b;

    if (p->x / 8 != q->x / 8) return p->x - q->x;
    return p->y - q->y;
}

/* A tile's name: short, no digits (the value goes on its line) */
static bool is_name(const char *t) {
    if (strlen(t) > 18) return false;
    for (; *t; t++)
        if (*t >= '0' && *t <= '9') return false;
    return true;
}

void voice_lines_clear(void) {
    lines_n = line_at = 0;
}

void voice_lines_set(lv_obj_t *obj, const char *head) {
    static piece_t p[PIECES_MAX];
    int            n = 0;
    char           first[LINE_LEN * 2];

    voice_lines_clear();
    if (!obj) return;
    /* the labels' real text: before a layout, one cut with "..." holds nothing else (lv_label LONG_DOT) */
    lv_obj_update_layout(obj);
    gather(obj, p, &n, 0);
    qsort(p, n, sizeof(p[0]), by_place);
    for (int i = 0; i < n && lines_n < LINES_MAX; i++) {
        char clean[LINE_LEN];

        voice_clean_text(p[i].text, clean, sizeof(clean));
        if (!clean[0]) continue;
        /* a name with its value just under it, in the same column: one line */
        if (is_name(clean) && i + 1 < n && p[i + 1].x / 8 == p[i].x / 8 && p[i + 1].y - p[i].y <= 30) {
            char value[LINE_LEN];

            voice_clean_text(p[i + 1].text, value, sizeof(value));
            snprintf(lines[lines_n++], LINE_LEN, "%s, %s", clean, value);
            i++;
            continue;
        }
        snprintf(lines[lines_n++], LINE_LEN, "%s", clean);
    }
    if (!voice_auto_active()) return;
    if (!lines_n) {
        voice_say_text_fmt("%s, nothing to read", head ? head : "");
        return;
    }
    snprintf(first, sizeof(first), "%s%s%d line%s. Small knob: line by line. %s", head ? head : "", head ? ", " : "",
             lines_n, lines_n == 1 ? "" : "s", lines[0]);
    voice_say_text_fmt("%s", first);
}

void voice_lines_begin(void) {
    voice_lines_clear();
}

void voice_lines_add(const char *text) {
    if (!text || lines_n >= LINES_MAX) return;
    voice_clean_text(text, lines[lines_n], LINE_LEN);
    if (lines[lines_n][0]) lines_n++;
}

void voice_lines_done(const char *head) {
    char first[LINE_LEN * 2];

    if (!voice_auto_active()) return;
    if (!lines_n) {
        voice_say_text_fmt("%s, nothing to read", head ? head : "");
        return;
    }
    snprintf(first, sizeof(first), "%s%s%d line%s. Small knob: line by line. %s", head ? head : "", head ? ", " : "",
             lines_n, lines_n == 1 ? "" : "s", lines[0]);
    voice_say_text_fmt("%s", first);
}

void voice_lines_step(int dir) {
    if (!lines_n || !voice_auto_active()) return;
    line_at += dir > 0 ? 1 : -1;
    if (line_at < 0) line_at = 0;
    if (line_at >= lines_n) line_at = lines_n - 1;
    voice_delay_say_text_fmt("%s, %d of %d", lines[line_at], line_at + 1, lines_n);
}

/* "3 of 7" for an entry of an LVGL list */
static void list_position(lv_obj_t *obj, char *buf, size_t size) {
    lv_obj_t *parent = lv_obj_get_parent(obj);

    if (!parent || !lv_obj_check_type(parent, &lv_list_class)) {
        return;
    }

    int      index = 0;
    int      total = 0;
    uint32_t n = lv_obj_get_child_cnt(parent);

    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(parent, i);

        if (lv_obj_check_type(c, &lv_list_btn_class)) {
            total++;
            if (c == obj) {
                index = total;
            }
        }
    }
    if (index > 0 && total > 1) {
        char pos[24];

        snprintf(pos, sizeof(pos), "%d of %d", index, total);
        append(buf, size, pos);
    }
}

/* The name of obj's row in a grid (Settings): the label in the same grid row, in front of it. Found
 * from the grid's child obj is in (obj itself or one of its parents) */
static const char *row_name_raw(lv_obj_t *obj);

/* Names given by the windows (voice_focus_set_name) */
#define NAMES_N 32

static struct {
    lv_obj_t *obj;
    char      name[48];
} names[NAMES_N];

static void name_deleted_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);

    for (int i = 0; i < NAMES_N; i++) {
        if (names[i].obj == obj) names[i].obj = NULL;
    }
}

void voice_focus_set_name(lv_obj_t *obj, const char *name) {
    int free_i = -1;

    if (!obj) return;
    for (int i = 0; i < NAMES_N; i++) {
        if (names[i].obj == obj) {
            snprintf(names[i].name, sizeof(names[i].name), "%s", name ? name : "");
            return;
        }
        if (!names[i].obj && free_i < 0) free_i = i;
    }
    if (free_i < 0) return;
    names[free_i].obj = obj;
    snprintf(names[free_i].name, sizeof(names[free_i].name), "%s", name ? name : "");
    lv_obj_add_event_cb(obj, name_deleted_cb, LV_EVENT_DELETE, NULL);
}

/* The grid cell obj is in (obj or a parent whose parent lays out a grid), NULL for none */
static lv_obj_t *grid_cell_of(lv_obj_t *obj) {
    lv_obj_t *cell = obj;

    while (cell && lv_obj_get_parent(cell) && lv_obj_get_style_layout(lv_obj_get_parent(cell), 0) != LV_LAYOUT_GRID) {
        cell = lv_obj_get_parent(cell);
    }
    return cell && lv_obj_get_parent(cell) ? cell : NULL;
}

/* The row's name, or the part of it for this control: a row "Line-in, Line-out" with two controls calls the first
 * "Line-in" and the second "Line-out" (the parts and the controls counted alike, else the whole name) */
static const char *row_name(lv_obj_t *obj) {
    static char part[64];
    const char *full;
    lv_obj_t   *cell, *grid;

    for (int i = 0; i < NAMES_N; i++) {
        if (names[i].obj == obj && names[i].name[0]) return names[i].name;
    }
    full = row_name_raw(obj);
    if (!full || !strstr(full, ", ") || !(cell = grid_cell_of(obj))) return full;

    grid    = lv_obj_get_parent(cell);
    int row = lv_obj_get_style_grid_cell_row_pos(cell, 0);
    int col = lv_obj_get_style_grid_cell_column_pos(cell, 0);
    int controls = 0, idx = 0, parts = 1;

    for (uint32_t i = 0; i < lv_obj_get_child_cnt(grid); i++) {
        lv_obj_t *c = lv_obj_get_child(grid, i);

        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN) || lv_obj_get_style_grid_cell_row_pos(c, 0) != row ||
            lv_obj_get_style_grid_cell_column_pos(c, 0) < 1 || lv_obj_check_type(c, &lv_label_class)) {
            continue;
        }
        controls++;
        if (lv_obj_get_style_grid_cell_column_pos(c, 0) < col) idx++;
    }
    for (const char *p = full; (p = strstr(p, ", ")) != NULL; p += 2) parts++;
    if (controls < 2 || controls != parts) return full;

    const char *s = full;

    for (int k = 0; k < idx; k++) s = strstr(s, ", ") + 2;
    const char *e = strstr(s, ", ");
    size_t      n = e ? (size_t)(e - s) : strlen(s);

    snprintf(part, sizeof(part), "%.*s", (int)(n < sizeof(part) - 1 ? n : sizeof(part) - 1), s);
    return part;
}

/* "3 of 12": the place of obj among the knob's stops of its page (the grids under one parent: a Settings tab), in
 * their order on it */
static void count_stops(lv_obj_t *o, lv_obj_t *obj, int *n, int *at) {
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(o); i++) {
        lv_obj_t *c = lv_obj_get_child(o, i);

        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
        if (lv_obj_get_group(c)) {
            (*n)++;
            if (c == obj) *at = *n;
        }
        count_stops(c, obj, n, at);
    }
}

static void page_position(lv_obj_t *obj, char *buf, size_t size) {
    lv_obj_t *cell = grid_cell_of(obj);

    if (!cell) return;
    lv_obj_t *page = lv_obj_get_parent(lv_obj_get_parent(cell));     /* above the grid: the page of its cards */

    if (!page) return;
    int n = 0, at = 0;

    count_stops(page, obj, &n, &at);
    if (at > 0 && n > 1) {
        char pos[24];

        snprintf(pos, sizeof(pos), "%d of %d", at, n);
        append(buf, size, pos);
    }
}

static const char *row_name_raw(lv_obj_t *obj) {
    lv_obj_t *cell = obj;

    while (cell && lv_obj_get_parent(cell) && lv_obj_get_style_layout(lv_obj_get_parent(cell), 0) != LV_LAYOUT_GRID) {
        cell = lv_obj_get_parent(cell);
    }
    if (!cell || !lv_obj_get_parent(cell)) {
        /* No grid: a row of a label and then the control, the label its name. obj or its parent in the row */
        for (lv_obj_t *o = obj; o && lv_obj_get_parent(o); o = lv_obj_get_parent(o)) {
            lv_obj_t *row = lv_obj_get_parent(o);

            if (lv_obj_get_style_layout(row, 0) != LV_LAYOUT_FLEX ||
                lv_obj_get_style_flex_flow(row, 0) != LV_FLEX_FLOW_ROW) {
                if (o != obj) {
                    break;      /* two levels at most: obj, or the box it sits in */
                }
                continue;
            }
            lv_obj_t *first = lv_obj_get_child(row, 0);

            if (first && first != o && lv_obj_check_type(first, &lv_label_class) &&
                !lv_obj_has_flag(first, LV_OBJ_FLAG_HIDDEN)) {
                const char *t = lv_label_get_text(first);

                if (t && t[0]) {
                    return t;
                }
            }
            break;
        }
        return NULL;
    }

    lv_obj_t *grid = lv_obj_get_parent(cell);
    int       row = lv_obj_get_style_grid_cell_row_pos(cell, 0);
    int       col = lv_obj_get_style_grid_cell_column_pos(cell, 0);

    for (uint32_t i = 0; i < lv_obj_get_child_cnt(grid); i++) {
        lv_obj_t *c = lv_obj_get_child(grid, i);

        if (c == cell || !lv_obj_check_type(c, &lv_label_class) || lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        if (lv_obj_get_style_grid_cell_row_pos(c, 0) == row && lv_obj_get_style_grid_cell_column_pos(c, 0) < col) {
            const char *t = lv_label_get_text(c);

            if (t && t[0]) {
                return t;
            }
        }
    }
    return NULL;
}

/* ---- Tables that say their rows themselves (voice_focus_set_row_cb) ---- */

#define ROW_CBS_N 8

static struct {
    lv_obj_t           *obj;
    voice_row_text_cb_t cb;
} row_cbs[ROW_CBS_N];

static voice_row_text_cb_t row_cb_find(lv_obj_t *obj) {
    for (int i = 0; i < ROW_CBS_N; i++) {
        if (row_cbs[i].obj == obj) {
            return row_cbs[i].cb;
        }
    }
    return NULL;
}

static void row_cb_deleted(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);

    for (int i = 0; i < ROW_CBS_N; i++) {
        if (row_cbs[i].obj == obj) {
            row_cbs[i].obj = NULL;
        }
    }
}

void voice_focus_set_row_cb(lv_obj_t *table, voice_row_text_cb_t cb) {
    int free_i = -1;

    if (!table) {
        return;
    }
    for (int i = 0; i < ROW_CBS_N; i++) {
        if (row_cbs[i].obj == table) {
            row_cbs[i].cb = cb;
            return;
        }
        if (!row_cbs[i].obj && free_i < 0) {
            free_i = i;
        }
    }
    if (free_i >= 0) {
        row_cbs[free_i].obj = table;
        row_cbs[free_i].cb  = cb;
        lv_obj_add_event_cb(table, row_cb_deleted, LV_EVENT_DELETE, NULL);
    }
}

/* ---- The on-screen keyboard (text_popup.c: callsign, locator, WiFi password, messages) ---- */

/* What a key of LVGL's keyboard is called aloud. Its punctuation by name: the cleaning for speech
 * drops most of it, and RHVoice says nothing for a lone symbol */
static const char *key_name(const char *t, char *buf, size_t size) {
    static const struct {
        const char *key, *name;
    } names[] = {
        {LV_SYMBOL_BACKSPACE, "backspace"}, {LV_SYMBOL_NEW_LINE, "enter"},   {LV_SYMBOL_KEYBOARD, "hide keyboard"},
        {LV_SYMBOL_LEFT, "cursor left"},    {LV_SYMBOL_RIGHT, "cursor right"}, {LV_SYMBOL_OK, "done"},
        {LV_SYMBOL_CLOSE, "cancel"},        {"1#", "numbers and symbols"},   {"abc", "lower case"},
        {"ABC", "upper case"},              {" ", "space"},                  {".", "dot"},
        {",", "comma"},                     {"-", "dash"},                   {"_", "underscore"},
        {"@", "at"},                        {"#", "hash"},                   {"$", "dollar"},
        {"%", "percent"},                   {"&", "and"},                    {"*", "star"},
        {"(", "left paren"},                {")", "right paren"},            {"+", "plus"},
        {"=", "equals"},                    {"/", "slash"},                  {"\\", "backslash"},
        {"?", "question mark"},             {"!", "exclamation"},            {":", "colon"},
        {";", "semicolon"},                 {"'", "apostrophe"},             {"\"", "quote"},
        {"<", "less than"},                 {">", "greater than"},           {"[", "left bracket"},
        {"]", "right bracket"},             {"{", "left brace"},             {"}", "right brace"},
        {"~", "tilde"},                     {"^", "caret"},                  {"|", "bar"},
        {"`", "back tick"},                 {"+/-", "plus minus"},
    };

    if (!t || !t[0]) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(t, names[i].key) == 0) {
            return names[i].name;
        }
    }
    /* A capital said so: a password's case matters */
    if (t[1] == '\0' && t[0] >= 'A' && t[0] <= 'Z') {
        snprintf(buf, size, "cap %c", t[0]);
        return buf;
    }
    return t;
}

static bool is_keys(lv_obj_t *obj) {
    return lv_obj_has_class(obj, &lv_btnmatrix_class);
}

/* The key the knob is on */
static const char *key_selected(lv_obj_t *obj, char *buf, size_t size) {
    uint16_t id = lv_btnmatrix_get_selected_btn(obj);

    if (id == LV_BTNMATRIX_BTN_NONE) {
        return NULL;
    }
    return key_name(lv_btnmatrix_get_btn_text(obj, id), buf, size);
}

/* The value of a control as it reads now; false for an object without one */
static bool value_text(lv_obj_t *obj, char *buf, size_t size) {
    buf[0] = '\0';

    if (lv_obj_check_type(obj, &lv_switch_class)) {
        snprintf(buf, size, "%s", lv_obj_has_state(obj, LV_STATE_CHECKED) ? "on" : "off");
    } else if (lv_obj_check_type(obj, &lv_checkbox_class)) {
        snprintf(buf, size, "%s", lv_obj_has_state(obj, LV_STATE_CHECKED) ? "checked" : "not checked");
    } else if (lv_obj_check_type(obj, &lv_dropdown_class)) {
        /* Open: the option the knob is on. LVGL's lv_dropdown_get_selected_str() gives the one chosen before the
         * list opened (sel_opt_id_orig), so turning through Country or Zone said "-" or "UTC" at every step */
        if (lv_dropdown_is_open(obj)) {
            const char *o = lv_dropdown_get_options(obj);
            uint16_t    want = ((lv_dropdown_t *)obj)->sel_opt_id, line = 0;
            size_t      n = 0;

            for (; o && *o && line < want; o++) {
                if (*o == '\n') line++;
            }
            while (o && *o && *o != '\n' && n + 1 < size) {
                buf[n++] = *o++;
            }
            buf[n] = '\0';
            if (n + 20 < size) {
                snprintf(buf + n, size - n, ", %u of %u", (unsigned)want + 1, (unsigned)lv_dropdown_get_option_cnt(obj));
            }
        } else {
            lv_dropdown_get_selected_str(obj, buf, size);
        }
#if LV_USE_ROLLER
    } else if (lv_obj_check_type(obj, &lv_roller_class)) {
        lv_roller_get_selected_str(obj, buf, size);
#endif
#if LV_USE_SPINBOX
    } else if (lv_obj_check_type(obj, &lv_spinbox_class)) {
        /* "+00030" said "30", "-0012.5" "minus 12.5": the padding is for the eye */
        const char *t = lv_textarea_get_text(obj);
        bool        neg = t && *t == '-';

        while (t && (*t == '+' || *t == '-' || *t == ' ')) t++;
        while (t && t[0] == '0' && t[1] >= '0' && t[1] <= '9') t++;
        snprintf(buf, size, "%s%s", neg ? "minus " : "", t ? t : "");
#endif
    } else if (lv_obj_check_type(obj, &lv_slider_class)) {
        /* A Settings slider shows its value as text beside it, in the same cell ("100%", "0.5 dB"): that,
         * else the slider's own step */
        lv_obj_t *cell = lv_obj_get_parent(obj);

        for (uint32_t i = 0; cell && i < lv_obj_get_child_cnt(cell); i++) {
            lv_obj_t *c = lv_obj_get_child(cell, i);

            if (lv_obj_check_type(c, &lv_label_class)) {
                snprintf(buf, size, "%s", lv_label_get_text(c));
                return true;
            }
        }
        if (lv_slider_get_mode(obj) == LV_SLIDER_MODE_RANGE)
            snprintf(buf, size, "from %d to %d", (int)lv_slider_get_left_value(obj), (int)lv_slider_get_value(obj));
        else
            snprintf(buf, size, "%d", (int)lv_slider_get_value(obj));
    } else if (lv_obj_check_type(obj, &lv_textarea_class)) {
        const char *text = lv_textarea_get_text(obj);

        if (text && text[0]) {
            snprintf(buf, size, "%s", lv_textarea_get_password_mode(obj) ? "hidden text" : text);
        } else {
            const char *hint = lv_textarea_get_placeholder_text(obj);

            snprintf(buf, size, "%s", hint && hint[0] ? hint : "empty");
        }
    } else if (lv_obj_check_type(obj, &lv_table_class)) {
        uint16_t row;
        uint16_t col;

        lv_table_get_selected_cell(obj, &row, &col);
        if (row != LV_TABLE_CELL_NONE && row < lv_table_get_row_cnt(obj) && col < lv_table_get_col_cnt(obj)) {
            voice_row_text_cb_t cb = row_cb_find(obj);

            if (cb) {
                cb(obj, row, buf, size);
            } else {
                snprintf(buf, size, "%s", lv_table_get_cell_value(obj, row, col));
            }
        }
    } else {
        return false;
    }
    return true;
}

/* ---- Texts given by the windows (voice_focus_set_text) ---- */

#define TEXTS_N 48

static struct {
    lv_obj_t *obj;
    char      text[160];
} texts[TEXTS_N];

static int text_find(lv_obj_t *obj) {
    for (int i = 0; i < TEXTS_N; i++) {
        if (texts[i].obj == obj) {
            return i;
        }
    }
    return -1;
}

static void text_deleted_cb(lv_event_t *e) {
    int i = text_find(lv_event_get_target(e));

    if (i >= 0) {
        texts[i].obj = NULL;
    }
}

void voice_focus_set_text(lv_obj_t *obj, const char *text) {
    int i;

    if (!obj) {
        return;
    }
    i = text_find(obj);
    if (text && text[0] && i < 0 && !voice_is_on()) {
        return;     /* speech off: nothing kept (the lists refresh every second) */
    }
    if (!text || !text[0]) {
        if (i >= 0) {
            texts[i].obj = NULL;
            lv_obj_remove_event_cb(obj, text_deleted_cb);
        }
        return;
    }
    if (i < 0) {
        i = text_find(NULL);
        if (i < 0) {
            return;     /* full: the reader's own description then */
        }
        texts[i].obj = obj;
        lv_obj_add_event_cb(obj, text_deleted_cb, LV_EVENT_DELETE, NULL);
    }
    snprintf(texts[i].text, sizeof(texts[i].text), "%s", text);
}

/* "2 of 5" for the selected row of a table of more than one */
static void table_position(lv_obj_t *obj, char *buf, size_t size) {
    uint16_t row, col;

    lv_table_get_selected_cell(obj, &row, &col);
    if (row != LV_TABLE_CELL_NONE && lv_table_get_row_cnt(obj) > 1) {
        char pos[24];

        snprintf(pos, sizeof(pos), "%u of %u", (unsigned)row + 1, (unsigned)lv_table_get_row_cnt(obj));
        append(buf, size, pos);
    }
}

bool voice_focus_describe(lv_obj_t *obj, char *buf, size_t size) {
    char value[128];

    if (!obj || !buf || size == 0) {
        return false;
    }
    buf[0] = '\0';

    int t = text_find(obj);

    if (t >= 0) {
        append(buf, size, texts[t].text);
        return true;
    }
    append(buf, size, row_name(obj));

    if (is_keys(obj)) {
        char        key[32];
        const char *k = key_selected(obj, key, sizeof(key));

#if LV_USE_KEYBOARD
        if (lv_obj_check_type(obj, &lv_keyboard_class)) {
            lv_obj_t *ta = lv_keyboard_get_textarea(obj);

            append(buf, size, "keyboard");
            if (ta) {
                value_text(ta, value, sizeof(value));
                append(buf, size, value);
            }
        }
#endif
        append(buf, size, k);
    } else if (lv_obj_check_type(obj, &lv_textarea_class)) {
        append(buf, size, "edit text");
        value_text(obj, value, sizeof(value));
        append(buf, size, value);
    } else if (lv_obj_check_type(obj, &lv_checkbox_class)) {
        append(buf, size, lv_checkbox_get_text(obj));
        value_text(obj, value, sizeof(value));
        append(buf, size, value);
    } else if (value_text(obj, value, sizeof(value))) {
        append(buf, size, value);
        if (lv_obj_check_type(obj, &lv_table_class)) {
            table_position(obj, buf, size);
        }
    } else if (lv_obj_check_type(obj, &lv_label_class)) {
        char clean[200];

        voice_clean_text(lv_label_get_text(obj), clean, sizeof(clean));
        append(buf, size, clean);
    } else {
        size_t start = strlen(buf);

        collect_labels(obj, buf, size, 0);
        /* A whole window focused (an app not wired for speech yet): every label on it was a long list of
         * numbers and names. Its first, usually the title, is enough */
        if (lv_obj_get_child_cnt(obj) > 4 && strlen(buf + start) > 120) {
            char *comma = strstr(buf + start, ", ");

            if (comma) {
                *comma = '\0';
            }
        }
        list_position(obj, buf, size);
    }
    if (lv_obj_has_state(obj, LV_STATE_DISABLED)) {
        append(buf, size, "unavailable");
    }
    if (buf[0]) {
        page_position(obj, buf, size);
    }
    return buf[0] != '\0';
}

/* ---- The value as it changes ---- */

static void say_value(lv_obj_t *obj) {
    char        value[128];
    char        prompt[96];
    const char *name;

    if (!voice_auto_active() || !value_text(obj, value, sizeof(value)) || !value[0]) {
        return;
    }
    name = row_name(obj);
    if (lv_obj_check_type(obj, &lv_checkbox_class)) {
        name = lv_checkbox_get_text(obj);
    }
    if (name && name[0]) {
        char clean[96];

        voice_clean_text(name, clean, sizeof(clean));
        snprintf(prompt, sizeof(prompt), "%s,", clean);
    } else {
        prompt[0] = '\0';
    }

    char clean_value[160];

    voice_clean_text(value, clean_value, sizeof(clean_value));
    if (lv_obj_check_type(obj, &lv_table_class)) {
        static lv_obj_t *last_obj;
        static char      last[160];

        table_position(obj, clean_value, sizeof(clean_value));
        if (obj == last_obj && strcmp(last, clean_value) == 0) {
            return;     /* refreshed, not moved */
        }
        last_obj = obj;
        snprintf(last, sizeof(last), "%s", clean_value);
    }
    voice_say_prompted_fmt(prompt, " %s", clean_value);
}

static bool focus_muted;

void voice_focus_mute(bool on) {
    focus_muted = on;
}

static void value_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);

    if (focus_muted) {
        return;
    }

    /* The keyboard: each key as the knob reaches it, and the key typed */
    if (is_keys(obj)) {
        lv_event_code_t code = lv_event_get_code(e);
        char            key[32];
        const char     *k;

        if (!voice_auto_active()) {
            return;
        }
        if (code == LV_EVENT_KEY) {
            uint32_t c = lv_event_get_key(e);

            if (c == LV_KEY_LEFT || c == LV_KEY_RIGHT || c == LV_KEY_UP || c == LV_KEY_DOWN) {
                k = key_selected(obj, key, sizeof(key));
                if (k) {
                    voice_delay_say_text_fmt("%s", k);
                }
            }
        } else if (code == LV_EVENT_VALUE_CHANGED) {
            k = key_selected(obj, key, sizeof(key));
            if (k) {
                voice_say_text_fmt("%s", k);
            }
        }
        return;
    }
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        say_value(obj);
        return;
    }
    if (lv_event_get_code(e) == LV_EVENT_KEY && lv_obj_check_type(obj, &lv_table_class)) {
        uint32_t key = lv_event_get_key(e);

        if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT || key == LV_KEY_UP || key == LV_KEY_DOWN) {
            say_value(obj);
        }
        return;
    }
    /* An open dropdown moves its highlight on the keys and says nothing until it closes */
    if (lv_event_get_code(e) == LV_EVENT_KEY && lv_obj_check_type(obj, &lv_dropdown_class) &&
        lv_dropdown_is_open(obj)) {
        uint32_t key = lv_event_get_key(e);

        if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT || key == LV_KEY_UP || key == LV_KEY_DOWN) {
            say_value(obj);
        }
    }
}

/* Once on each object the knob reaches: removed first, so it is never there twice */
static void dropdown_esc_cb(lv_event_t *e) {
    lv_obj_t *dd = lv_event_get_target(e);

    if (lv_event_get_key(e) != LV_KEY_ESC || !lv_dropdown_is_open(dd)) return;
    ((lv_dropdown_t *)dd)->sel_opt_id = ((lv_dropdown_t *)dd)->sel_opt_id_orig;
    lv_dropdown_close(dd);
    lv_event_stop_processing(e);    /* before LVGL's own handler and the window's: the window stays */
    if (voice_auto_active()) {
        char v[128];

        lv_dropdown_get_selected_str(dd, v, sizeof(v));
        voice_say_clean(v, false);
    }
}

static void watch_value(lv_obj_t *obj) {
    lv_obj_remove_event_cb(obj, value_cb);
    lv_obj_add_event_cb(obj, value_cb, LV_EVENT_ALL, NULL);
    if (lv_obj_check_type(obj, &lv_dropdown_class)) {
        lv_obj_remove_event_cb(obj, dropdown_esc_cb);
        lv_obj_add_event_cb(obj, dropdown_esc_cb, (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), NULL);
    }
}

/* ---- The hint ---- */

static char        hint[800];
static lv_timer_t *hint_timer;

static void hint_clear(void) {
    hint[0] = '\0';
    if (hint_timer) {
        lv_timer_del(hint_timer);
        hint_timer = NULL;
    }
}

/* No focus change came with the hint: said alone */
static void hint_timer_cb(lv_timer_t *t) {
    (void)t;
    hint_timer = NULL;
    if (hint[0] && voice_auto_active()) {
        voice_say_clean(hint, false);
    }
    hint[0] = '\0';
}

void voice_focus_hint(const char *text) {
    if (!text || !text[0] || !voice_auto_active()) {
        return;
    }
    snprintf(hint, sizeof(hint), "%s", text);
    if (!hint_timer) {
        hint_timer = lv_timer_create(hint_timer_cb, 0, NULL);
        lv_timer_set_repeat_count(hint_timer, 1);
    }
}

void voice_say_label_change(lv_obj_t *label, char *last, size_t size, const char *prefix) {
    char now[300];

    if (!label || !last || !size || !voice_is_on()) return;
    voice_clean_text(lv_label_get_text(label), now, sizeof(now));
    if (!strcmp(now, last)) return;
    snprintf(last, size, "%s", now);
    if (!now[0] || !voice_auto_active()) return;
    if (prefix && prefix[0])
        voice_say_text_fmt("%s, %s", prefix, now);
    else
        voice_say_text_fmt("%s", now);
}

bool voice_focus_hint_waiting(void) {
    return hint[0] != '\0';
}

void voice_focus_hint_add(const char *text) {
    if (!text || !text[0] || !voice_auto_active()) {
        return;
    }
    if (!hint[0]) {
        voice_focus_hint(text);
        return;
    }
    size_t n = strlen(hint);

    snprintf(hint + n, sizeof(hint) - n, ". %s", text);
}

void voice_focus_cb(lv_group_t *group) {
    if (!voice_auto_active()) {
        hint_clear();
        return;
    }

    lv_obj_t *obj = lv_group_get_focused(group);
    char      buf[256];
    char      text[1060];

    voice_trace("focus %p class %p quiet %i", (void *)obj, obj ? (const void *)lv_obj_get_class(obj) : NULL,
                obj ? lv_obj_has_flag(obj, VOICE_FOCUS_QUIET) : 0);
    if (obj && lv_obj_has_flag(obj, VOICE_FOCUS_QUIET)) {
        /* The window speaks its items itself; its name, if one came, still said (a window that says its own
         * name at once cuts this off) */
        if (hint[0]) {
            char said[sizeof(hint)];

            snprintf(said, sizeof(said), "%s", hint);
            hint_clear();
            voice_say_clean(said, false);
        }
        return;
    }
    if (!obj || !voice_focus_describe(obj, buf, sizeof(buf))) {
        buf[0] = '\0';
    }
    if (obj) {
        watch_value(obj);
    }
    if (hint[0]) {
        snprintf(text, sizeof(text), "%s%s%s", hint, buf[0] ? ". " : "", buf);
        hint_clear();
        voice_say_clean(text, false);
    } else if (buf[0]) {
        voice_say_clean(buf, true);
    }
}

void voice_focus_init(lv_group_t *group) {
    lv_group_set_focus_cb(group, voice_focus_cb);
}

void voice_focus_quiet(lv_obj_t *obj) {
    if (obj) {
        lv_obj_add_flag(obj, VOICE_FOCUS_QUIET);
    }
}
