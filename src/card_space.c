/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the room left on the card (see card_space.h, #341)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "card_space.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/statvfs.h>

#define MB (1024ull * 1024ull)
#define GB (1024ull * MB)

uint32_t card_warn_mb(int choice) {
    switch (choice) {
        case CARD_WARN_100MB: return 100;
        case CARD_WARN_250MB: return 250;
        case CARD_WARN_500MB: return 500;
        case CARD_WARN_1GB:   return 1024;
        default:              return 0;
    }
}

uint64_t card_warn_under(uint64_t total_bytes, int choice) {
    uint64_t under = card_warn_mb(choice) * MB;

    if (!under || !total_bytes) return 0;
    return under < total_bytes / 4 ? under : total_bytes / 4;
}

bool card_space_low(uint64_t free_bytes, uint64_t total_bytes, int choice) {
    return free_bytes < card_warn_under(total_bytes, choice);
}

void card_space_text(uint64_t bytes, char *out, size_t size) {
    if (bytes < 1000 * MB) {
        snprintf(out, size, "%u MB", (unsigned)(bytes / MB));
    } else if (bytes < 10 * GB) {
        unsigned tenths = (unsigned)(bytes * 10 / GB);   /* 1000 MB is "0.9 GB": down, never more than there is */

        snprintf(out, size, "%u.%u GB", tenths / 10, tenths % 10);
    } else {
        snprintf(out, size, "%u GB", (unsigned)(bytes / GB));
    }
}

void card_space_line(uint64_t free_bytes, uint64_t total_bytes, char *out, size_t size) {
    char a[24], b[24];

    card_space_text(free_bytes, a, sizeof(a));
    card_space_text(total_bytes, b, sizeof(b));
    snprintf(out, size, "%s free of %s", a, b);
}

bool card_space_read(const char *path, uint64_t *free_bytes, uint64_t *total_bytes) {
    struct statvfs fs;

#ifdef HOST_BUILD
    /* The PC build: HOST_STUB_CARD_MB=<free>,<total> stands in for every filesystem, as a card of that size */
    const char *e = getenv("HOST_STUB_CARD_MB");
    unsigned long f, t;

    if (e && sscanf(e, "%lu,%lu", &f, &t) == 2) {
        *free_bytes  = f * MB;
        *total_bytes = t * MB;
        return true;
    }
#endif
    if (statvfs(path, &fs) != 0 || !fs.f_blocks) return false;
    *free_bytes  = (uint64_t)fs.f_bavail * fs.f_frsize;
    *total_bytes = (uint64_t)fs.f_blocks * fs.f_frsize;
    return true;
}
