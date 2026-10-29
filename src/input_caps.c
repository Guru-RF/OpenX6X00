/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - What an input device has, from sysfs (input_caps.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "input_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysmacros.h>

/* A line of the input device behind the node dev: /sys/dev/char/M:m is its eventN, and device/ the
 * inputN with the capabilities and ids. Without its newline it was cut short: false */
static bool attr(dev_t dev, const char *name, char *line, size_t size) {
    char  path[96];
    FILE *f;
    bool  ok;

    snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/device/%s", major(dev), minor(dev), name);
    if (!(f = fopen(path, "r"))) return false;
    ok = fgets(line, size, f) && strchr(line, '\n');
    fclose(f);
    return ok;
}

bool input_caps_bits(dev_t dev, const char *what, unsigned long *bits, size_t n) {
    char name[32], line[1024];   /* KEY_MAX is 24 words of 8 digits on the radios, 12 of 16 on a PC */

    memset(bits, 0, n * sizeof(*bits));
    snprintf(name, sizeof(name), "capabilities/%s", what);
    if (!attr(dev, name, line, sizeof(line))) return false;
    /* Hex words of the kernel's long, the highest first and the empty ones before it left out
     * (input_print_bitmap): the last word is bits[0]. A 32-bit GUI on a 64-bit kernel gets 32-bit
     * words, so a word is always a long here */
    line[strcspn(line, "\n")] = '\0';
    for (size_t i = 0; i < n; i++) {
        char *w = strrchr(line, ' ');

        bits[i] = strtoul(w ? w + 1 : line, NULL, 16);
        if (!w) break;
        *w = '\0';
    }
    return true;
}

bool input_caps_id(dev_t dev, struct input_id *id) {
    static const char *const names[] = {"id/bustype", "id/vendor", "id/product", "id/version"};
    __u16                   *v[]     = {&id->bustype, &id->vendor, &id->product, &id->version};
    char                     line[16];

    *id = (struct input_id){0};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!attr(dev, names[i], line, sizeof(line))) {
            *id = (struct input_id){0};
            return false;
        }
        *v[i] = strtoul(line, NULL, 16);
    }
    return true;
}
