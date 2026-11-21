/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of a page button held (src/button_page.c, #317): the page before a page button's, and what
 *  a hold of a button does, on bars shaped as the GUI's are:
 *  - a page button with its prev set (the main screen's groups, JS8, PSK): that page;
 *  - only next links, as FT8's five pages, the cluster's two, the CW messages' three, the CW keyboard's 2 to 4 (its
 *    messages' pages as many as there are messages): from every page, the one before, round the ring;
 *  - a bar of one page leading to itself: that page; a button leading nowhere, a ring that is broken or never comes
 *    back to the page held: none (button_prev_page_cb then leaves the buttons as they are);
 *  - the hold: a button's own hold kept (none of the page buttons has another one now, a knob button's and the
 *    others' are theirs); a page button without one goes back; any other button without one does nothing.
 *  Up to v0.3.13 a page button without a hold of its own did nothing held: FT8, the cluster, the messages, the CW
 *  keyboard, and those with a press of their own (Settings, SSTV, WeFax, Field Ops, NAVTEX, given holds in their files).
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "button_bar.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;

static void check(bool ok, const char *what) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s\n", what);
    }
}

/* ---- button_bar.cpp's, stood in for: only their addresses count ---- */

void button_next_page_cb(button_data_t *data) {
    (void)data;
}

void button_prev_page_cb(button_data_t *data) {
    (void)data;
}

static void own_hold(button_data_t *data) {
    (void)data;
}

static void own_press(button_data_t *data) {
    (void)data;
}

/* ---- A bar of n pages, each with its page button first ---- */

#define MAX_PAGES 8

static buttons_page_t pages[MAX_PAGES];
static button_data_t  firsts[MAX_PAGES];

/* n pages in a ring by their next links only, prev left out */
static void ring(int n) {
    memset(pages, 0, sizeof(pages));
    memset(firsts, 0, sizeof(firsts));
    for (int i = 0; i < n; i++) {
        firsts[i].type     = BTN_TEXT;
        firsts[i].press    = button_next_page_cb;
        firsts[i].next     = &pages[(i + 1) % n];
        pages[i].items[0] = &firsts[i];
    }
}

static void rings(void) {
    char what[96];

    for (int n = 1; n <= MAX_PAGES; n++) {
        ring(n);
        for (int i = 0; i < n; i++) {
            snprintf(what, sizeof(what), "%d pages by next only: before page %d is page %d", n, i + 1,
                     (i + n - 1) % n + 1);
            check(buttons_page_before(&firsts[i]) == &pages[(i + n - 1) % n], what);
            snprintf(what, sizeof(what), "%d pages: page %d's button held goes back", n, i + 1);
            check(buttons_hold_fn(&firsts[i]) == button_prev_page_cb, what);
        }
    }
}

/* prev wins: a bar that set it (and its next) as the main screen's groups do */
static void prev_given(void) {
    ring(4);
    for (int i = 0; i < 4; i++)
        firsts[i].prev = &pages[(i + 3) % 4];
    firsts[2].prev = &pages[0];     /* a prev that is not the ring's: taken as set */
    check(buttons_page_before(&firsts[0]) == &pages[3], "prev set: page 1's is page 4");
    check(buttons_page_before(&firsts[2]) == &pages[0], "prev set: taken as it is set");
}

/* The CW keyboard: page 1, its messages' pages (none to two), the Live page, back to page 1 */
static void cw_keyboard(void) {
    for (int np = 0; np <= 2; np++) {
        char what[96];
        int  n = 2 + np;

        ring(n);
        snprintf(what, sizeof(what), "CW keyboard, %d pages: page 1's is the Live page", n);
        check(buttons_page_before(&firsts[0]) == &pages[n - 1], what);
        snprintf(what, sizeof(what), "CW keyboard, %d pages: the Live page's is the one before it", n);
        check(buttons_page_before(&firsts[n - 1]) == &pages[n - 2], what);
    }
}

static void broken(void) {
    ring(3);
    firsts[1].next = NULL;      /* page 2's button leads nowhere */
    check(buttons_page_before(&firsts[0]) == NULL, "a ring broken at page 2: none for page 1");

    ring(3);
    firsts[2].next = &pages[1]; /* 1 -> 2 -> 3 -> 2 -> 3 ...: never back to page 1 */
    check(buttons_page_before(&firsts[0]) == NULL, "a ring that never comes back: none, and the walk ends");

    ring(3);
    pages[1].items[0] = NULL;   /* a page with no page button */
    check(buttons_page_before(&firsts[0]) == NULL, "a page without its page button: none");

    button_data_t lone = {.type = BTN_TEXT, .label = "(BANDS 1:1)"};
    check(buttons_page_before(&lone) == NULL, "a 1:1 page button leading nowhere: none");
    check(buttons_hold_fn(&lone) == NULL, "a 1:1 page button with no press: its hold does nothing");
    check(buttons_page_before(NULL) == NULL && buttons_hold_fn(NULL) == NULL, "no button: nothing");
}

static void holds(void) {
    ring(3);
    firsts[1].hold = own_hold;
    check(buttons_hold_fn(&firsts[1]) == own_hold, "a page button's own hold kept");

    button_data_t knob  = {.type = BTN_TEXT, .press = own_press, .hold = own_hold};
    button_data_t plain = {.type = BTN_TEXT, .press = own_press};
    button_data_t page  = {.type = BTN_TEXT, .press = own_press, .next = &pages[0]};   /* a page press of its own */

    check(buttons_hold_fn(&knob) == own_hold, "another button's hold kept");
    check(buttons_hold_fn(&plain) == NULL, "another button without a hold: nothing");
    check(buttons_hold_fn(&page) == NULL, "a page button with a press of its own and no hold: its file gives one");
}

int main(void) {
    rings();
    prev_given();
    cw_keyboard();
    broken();
    holds();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
