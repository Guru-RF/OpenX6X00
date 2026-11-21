/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - a page button held goes one page back (#317): the page before a page button's, and what a hold of
 *  a button does. button_bar.h has them; on their own here for the host test (tools/page_back)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "button_bar.h"

#define RING_MAX 16     /* pages a bar may have: the most any has now is 6 */

buttons_page_t *buttons_page_before(const button_data_t *data) {
    buttons_page_t *p;

    if (!data) return NULL;
    if (data->prev) return data->prev;
    p = data->next;
    for (int i = 0; p && i < RING_MAX; i++) {
        const button_data_t *first = p->items[0];

        if (!first || !first->next) return NULL;    /* a page whose button leads nowhere: no way round */
        if (first->next->items[0] == data) return p;
        p = first->next;
    }
    return NULL;    /* the links never come back to data's page */
}

void (*buttons_hold_fn(const button_data_t *data))(button_data_t *) {
    if (!data) return NULL;
    if (data->hold) return data->hold;
    return data->press == button_next_page_cb ? button_prev_page_cb : NULL;
}
