/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the first-start guide's Keyboard step: what it shows (see first_start_kbd.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "first_start_kbd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The code bt_dev's agent asks to type: its activity "type 123456 and Enter on the keyboard" (bt_dev.c). A PIN
 * code can have letters: what stands between "type " and the next space, into code; false none */
static bool passkey(const char *activity, char *code, size_t size) {
    size_t n;

    if (strncmp(activity, "type ", 5) != 0) {
        return false;
    }
    activity += 5;
    n = strcspn(activity, " ");
    if (n == 0 || n >= size) {
        return false;
    }
    memcpy(code, activity, n);
    code[n] = 0;
    return true;
}

/* What bt_dev's worker says it does, while it pairs or connects */
static fsk_pairing_t pairing_of(const char *activity) {
    if (strstr(activity, "pairing key")) {
        return FSK_PAIR_KEY;
    }
    if (strstr(activity, "searching")) {
        return FSK_PAIR_SEARCH;
    }
    if (strstr(activity, "pairing")) {
        return FSK_PAIR_PAIRING;
    }
    if (strstr(activity, "connecting")) {
        return FSK_PAIR_CONNECTING;
    }
    return FSK_PAIR_BUSY;
}

/* How the last pairing ended, from the activity bt_dev's worker left: none when it worked (it leaves "") */
static fsk_fail_t fail_of(const char *activity) {
    if (strstr(activity, "not found")) {
        return FSK_FAIL_NOT_FOUND;      /* "keyboard not found", "not found - is it in pairing mode?" */
    }
    if (strstr(activity, "connect failed")) {
        return FSK_FAIL_CONNECT;        /* "paired, connect failed", "connect failed" */
    }
    if (strstr(activity, "pairing failed")) {
        return FSK_FAIL_PAIR;
    }
    return FSK_FAIL_NONE;
}

void fsk_decide(const fsk_in_t *in, fsk_out_t *out) {
    const bt_adapter_t *a  = &in->adapter;
    bool                on = a->seen && a->bluez && a->present && a->powered;
    int                 connected = -1;

    memset(out, 0, sizeof(*out));
    for (int i = 0; i < in->n; i++) {
        const bt_dev_t *d = &in->devs[i];

        if (!d->input) {
            continue;
        }
        if (d->connected) {
            if (connected < 0) {
                connected = i;
            }
        } else if (out->n_shown < FSK_SHOWN) {
            out->shown[out->n_shown++] = i;
        }
    }
    if (connected >= 0) {
        snprintf(out->name, sizeof(out->name), "%s", in->devs[connected].name);
    }

    /* What a pairing asks comes first: the code is typed on the keyboard within seconds */
    if (passkey(in->activity, out->code, sizeof(out->code))) {
        out->state = FSK_PASSKEY;
    } else if (in->busy) {
        out->state   = FSK_PAIRING;
        out->pairing = pairing_of(in->activity);
    } else if (on && in->scanning) {
        out->state = FSK_SCANNING;
    } else if (in->kbd_ready) {
        out->state = FSK_WORKS;         /* a USB keyboard too (no name), with Bluetooth off or not there */
    } else if (!a->seen || (in->turning_on && !on)) {
        out->state = FSK_STARTING;
    } else if (!a->bluez) {
        out->state = FSK_NO_SERVICE;
    } else if (!a->present) {
        out->state = in->chip_on ? FSK_NO_ADAPTER : FSK_CHIP_OFF;
    } else if (!a->powered) {
        out->state = FSK_BT_OFF;
    } else if (connected >= 0) {
        out->state = FSK_CONNECTED;     /* its input node a moment after the link */
    } else if (in->pair_asked && (out->fail = fail_of(in->activity)) != FSK_FAIL_NONE) {
        out->state = FSK_FAILED;
    } else if (out->n_shown) {
        out->state = FSK_LIST;
    } else if (in->scan_asked) {
        out->state = FSK_NONE_FOUND;
    } else {
        out->state = FSK_READY;
    }
}

/* A keyboard of this name is paired: a keyboard that takes a new address at each pairing key is paired under the
 * next one, and the entry the scan found it at is gone for good (bt_dev.c forgets its old pairings by name too) */
static bool paired_as(const bt_dev_t *now, int n_now, const char *name) {
    for (int i = 0; i < n_now; i++) {
        if (now[i].input && now[i].paired && strcmp(now[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

int fsk_merge(fsk_kept_t *kept, const bt_dev_t *now, int n_now, bt_dev_t *out) {
    int n = 0;

    for (int i = 0; i < n_now && n < FSK_DEVS; i++) {
        out[n++] = now[i];
    }
    for (int k = 0; k < kept->n && n < FSK_DEVS; k++) {
        bool listed = false;

        for (int i = 0; i < n_now && !listed; i++) {
            listed = strcmp(now[i].addr, kept->dev[k].addr) == 0;
        }
        if (!listed && !paired_as(now, n_now, kept->dev[k].name)) {
            out[n++] = kept->dev[k];
        }
    }
    kept->n = 0;
    for (int i = 0; i < n && kept->n < BT_DEV_MAX; i++) {
        if (out[i].input && !out[i].paired) {
            kept->dev[kept->n++] = out[i];
        }
    }
    return n;
}

/* The bytes of the UTF-8 character that starts with c */
static size_t utf8_len(unsigned char c) {
    return c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
}

bool fsk_echo_add(char *line, size_t size, uint32_t key) {
    char   c[5] = {0};
    size_t n, len;

    if (key >= ' ' && key < 127) {
        c[0] = (char)key;
    } else {
        /* A letter as xkb gives it: its UTF-8 bytes in the key, the lead byte first. The radio's own keys (the VOL
         * knob's 0x290 on, X11 keysyms such as Scroll Lock's 0xFF14) start with no lead byte */
        memcpy(c, &key, sizeof(key));
        if ((unsigned char)c[0] < 0xC0 || strlen(c) != utf8_len((unsigned char)c[0])) {
            return false;
        }
    }
    n = strlen(c);
    if (n + 1 > size) {
        return false;
    }
    len = strlen(line);
    while (len + n + 1 > size) {    /* the oldest characters go */
        size_t first = utf8_len((unsigned char)line[0]);

        memmove(line, line + first, len - first + 1);
        len -= first;
    }
    memcpy(line + len, c, n + 1);
    return true;
}

bool fsk_echo_back(char *line) {
    size_t len = strlen(line);

    if (!len) {
        return false;
    }
    while (len > 0 && ((unsigned char)line[len - 1] & 0xC0) == 0x80) {
        len--;      /* the continuation bytes of a letter, then its lead byte */
    }
    line[len ? len - 1 : 0] = 0;
    return true;
}

/* ---- the PC build's screenshots ---- */

static bool flag(const char *v) {
    return atoi(v) != 0;
}

/* "AA:BB:CC:DD:EE:01|K380|input,paired" */
static void fake_dev(fsk_in_t *in, char *v) {
    char     *name = strchr(v, '|');
    char     *fl   = name ? strchr(name + 1, '|') : NULL;
    bt_dev_t *d;

    if (in->n >= FSK_DEVS || !name) {
        return;
    }
    d = &in->devs[in->n];
    memset(d, 0, sizeof(*d));
    *name++ = 0;
    if (fl) {
        *fl++ = 0;
        d->paired    = strstr(fl, "paired") != NULL;
        d->connected = strstr(fl, "connected") != NULL;
        d->input     = strstr(fl, "input") != NULL;
        d->audio     = strstr(fl, "audio") != NULL;
    }
    snprintf(d->addr, sizeof(d->addr), "%s", v);
    snprintf(d->name, sizeof(d->name), "%s", name);
    in->n++;
}

bool fsk_fake_load(const char *path, fsk_in_t *in, char *typed, size_t typed_size) {
    FILE *f = fopen(path, "r");
    char  line[160];

    if (!f) {
        return false;
    }
    memset(in, 0, sizeof(*in));
    in->chip_on = true;
    in->adapter = (bt_adapter_t){.seen = true, .bluez = true, .present = true, .powered = true};
    while (fgets(line, sizeof(line), f)) {
        char *v = strchr(line, '=');

        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '#' || !v) {
            continue;
        }
        *v++ = 0;
        if (!strcmp(line, "adapter")) {
            bt_adapter_t *a = &in->adapter;

            a->seen    = strcmp(v, "unseen") != 0;
            a->bluez   = a->seen && strcmp(v, "no-service") != 0;
            a->present = a->bluez && strcmp(v, "none") != 0 && strcmp(v, "chip-off") != 0;
            a->powered = a->present && strcmp(v, "off") != 0;
            in->chip_on = strcmp(v, "chip-off") != 0;
        } else if (!strcmp(line, "scanning")) {
            in->scanning = flag(v);
        } else if (!strcmp(line, "busy")) {
            in->busy = flag(v);
        } else if (!strcmp(line, "scan")) {
            in->scan_asked = flag(v);
        } else if (!strcmp(line, "pair")) {
            in->pair_asked = flag(v);
        } else if (!strcmp(line, "ready")) {
            in->kbd_ready = flag(v);
        } else if (!strcmp(line, "turning")) {
            in->turning_on = flag(v);
        } else if (!strcmp(line, "activity")) {
            snprintf(in->activity, sizeof(in->activity), "%s", v);
        } else if (!strcmp(line, "typed") && typed) {
            snprintf(typed, typed_size, "%s", v);
        } else if (!strcmp(line, "dev")) {
            fake_dev(in, v);
        }
    }
    fclose(f);
    return true;
}
