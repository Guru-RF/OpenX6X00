/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Settings opens where it was left (#362): see settings_place.h
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "settings_place.h"

#include <stdio.h>
#include <string.h>

/* obj is page or inside it */
static bool inside(lv_obj_t *obj, lv_obj_t *page) {
    for (; obj; obj = lv_obj_get_parent(obj)) {
        if (obj == page) return true;
    }
    return false;
}

/* What obj sits in on its card's grid: obj itself, or the cell or list around it. NULL off a grid */
static lv_obj_t *cell_of(lv_obj_t *obj) {
    while (obj && lv_obj_get_parent(obj) && lv_obj_get_style_layout(lv_obj_get_parent(obj), 0) != LV_LAYOUT_GRID) {
        obj = lv_obj_get_parent(obj);
    }
    return obj && lv_obj_get_parent(obj) ? obj : NULL;
}

lv_obj_t *settings_row_label(lv_obj_t *obj) {
    lv_obj_t *cell = cell_of(obj);

    if (!cell) return NULL;

    lv_obj_t *grid = lv_obj_get_parent(cell);
    int       row  = lv_obj_get_style_grid_cell_row_pos(cell, 0);

    for (uint32_t i = 0; i < lv_obj_get_child_cnt(grid); i++) {
        lv_obj_t *c = lv_obj_get_child(grid, i);

        if (lv_obj_has_flag(c, SETTINGS_ROW_LABEL) && lv_obj_get_style_grid_cell_row_pos(c, 0) == row) return c;
    }
    return NULL;
}

bool settings_reachable(lv_obj_t *obj) {
    if (!obj || lv_obj_has_state(obj, LV_STATE_DISABLED)) return false;
    for (; obj; obj = lv_obj_get_parent(obj)) {
        if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return false;
    }
    return true;
}

/* How many objects the knob can be on in obj, itself included */
static int controls_in(lv_obj_t *obj) {
    int n = lv_obj_get_group(obj) ? 1 : 0;

    for (uint32_t i = 0; i < lv_obj_get_child_cnt(obj); i++) {
        n += controls_in(lv_obj_get_child(obj, i));
    }
    return n;
}

/* The first text in obj: a list entry's name */
static const char *first_text(lv_obj_t *obj) {
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(obj); i++) {
        lv_obj_t   *c = lv_obj_get_child(obj, i);
        const char *t = lv_obj_check_type(c, &lv_label_class) ? lv_label_get_text(c) : first_text(c);

        if (t && t[0]) return t;
    }
    return "";
}

/* The card's title: the first text of its head, card_begin()'s first child */
static const char *card_title(lv_obj_t *grid) {
    lv_obj_t *head = lv_obj_get_child(grid, 0);
    lv_obj_t *lbl  = head ? lv_obj_get_child(head, 0) : NULL;

    return lbl && lv_obj_check_type(lbl, &lv_label_class) ? lv_label_get_text(lbl) : "";
}

/* What a control is called: its card, its row and, in a list of them (the networks, the devices), its entry's name.
 * Not a control's own text elsewhere: a button showing its value (the callsign) would not be found once it changed */
static void key_of(lv_obj_t *obj, char *out, size_t size) {
    lv_obj_t   *cell = cell_of(obj);
    lv_obj_t   *lbl  = settings_row_label(obj);
    const char *own  = cell && cell != obj && controls_in(cell) > 1 ? first_text(obj) : "";

    snprintf(out, size, "%s\x1f%s\x1f%s", cell ? card_title(lv_obj_get_parent(cell)) : "",
             lbl ? lv_label_get_text(lbl) : "", own);
}

#define EACH_OBJ(g, o) \
    for (lv_obj_t **o = (lv_obj_t **)_lv_ll_get_head(&(g)->obj_ll); o; o = (lv_obj_t **)_lv_ll_get_next(&(g)->obj_ll, o))

bool settings_place_get(settings_place_t *place, lv_obj_t *page, lv_group_t *g, lv_obj_t *obj) {
    char key[sizeof(place->key)];

    place->key[0]   = '\0';
    place->n        = 0;
    place->scroll_y = 0;
    if (!page || !g || !obj || obj == page || !inside(obj, page) || lv_obj_get_group(obj) != g) return false;

    key_of(obj, place->key, sizeof(place->key));
    EACH_OBJ(g, o) {
        if (*o == obj) break;
        if (!inside(*o, page)) continue;
        key_of(*o, key, sizeof(key));
        if (!strcmp(key, place->key)) place->n++;
    }
    place->scroll_y = lv_obj_get_scroll_y(page);
    return true;
}

lv_obj_t *settings_place_find(const settings_place_t *place, lv_obj_t *page, lv_group_t *g) {
    char key[sizeof(place->key)];
    int  n = 0;

    if (!place->key[0] || !page || !g) return NULL;
    EACH_OBJ(g, o) {
        if (!inside(*o, page)) continue;
        key_of(*o, key, sizeof(key));
        if (strcmp(key, place->key)) continue;
        if (n++ == place->n) return settings_reachable(*o) ? *o : NULL;
    }
    return NULL;
}
