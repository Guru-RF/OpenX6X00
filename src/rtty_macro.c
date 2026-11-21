/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - RTTY's macros (#312), see rtty_macro.h
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "rtty_macro.h"
#include "board.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef RTTY_MACROS_PATH
#define RTTY_MACROS_PATH BOARD_DATA "/rtty_macros.txt"
#endif

static rtty_macro_t macros[RTTY_MACRO_MAX];
static int          macro_count;

/* Copied, cut to the size, tabs and line breaks as spaces */
static void copy_clean(char *dst, size_t dst_sz, const char *src) {
    size_t o = 0;

    if (!src) src = "";
    for (const char *p = src; *p && o + 1 < dst_sz; p++) {
        char c = *p;

        if (c == '\t' || c == '\r' || c == '\n') c = ' ';
        dst[o++] = c;
    }
    dst[o] = '\0';
}

/* An RTTY QSO's usual overs: the CQ, the answer to one, the report, the thanks for it, the end, and a QRZ after a CQ
 * nobody was copied on. No hyphens: each would cost two shifts */
static void seed_default_macros(void) {
    macro_count = 0;
    rtty_macro_add("CQ",     "CQ CQ CQ DE <MYCALL> <MYCALL> <MYCALL> K");
    rtty_macro_add("Answer", "<CALL> DE <MYCALL> <MYCALL> <MYCALL> K");
    rtty_macro_add("Report", "<CALL> DE <MYCALL> TNX FOR THE CALL UR RST <RST> <RST> <CALL> DE <MYCALL> KN");
    rtty_macro_add("TNX",    "<CALL> DE <MYCALL> TNX FOR RST <RSTR> BTU <CALL> DE <MYCALL> KN");
    rtty_macro_add("73",     "<CALL> DE <MYCALL> TNX FER QSO 73 ES GL <CALL> DE <MYCALL> SK");
    rtty_macro_add("QRZ",    "QRZ? DE <MYCALL> <MYCALL> K");
}

void rtty_macros_load(void) {
    FILE *f = fopen(RTTY_MACROS_PATH, "r");
    char  line[RTTY_MACRO_NAME_LEN + RTTY_MACRO_TEXT_LEN + 8];

    macro_count = 0;
    if (!f) {
        seed_default_macros();
        rtty_macros_save();
        return;
    }
    while (macro_count < RTTY_MACRO_MAX && fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        char *tab;

        if (nl) *nl = '\0';
        tab = strchr(line, '\t');
        if (!line[0] || !tab) continue;
        *tab = '\0';
        copy_clean(macros[macro_count].name, sizeof(macros[0].name), line);
        copy_clean(macros[macro_count].text, sizeof(macros[0].text), tab + 1);
        macro_count++;
    }
    fclose(f);
}

bool rtty_macros_save(void) {
    FILE *f = fopen(RTTY_MACROS_PATH, "w");
    bool  ok;

    if (!f) return false;
    for (int i = 0; i < macro_count; i++) {
        fprintf(f, "%s\t%s\n", macros[i].name, macros[i].text);
    }
    /* /mnt is FAT on a card the battery's removal leaves without power: not left in the page cache */
    ok = fflush(f) == 0;
    if (ok) fsync(fileno(f));
    if (fclose(f) != 0) ok = false;
    return ok;
}

int rtty_macros_count(void) {
    return macro_count;
}

const rtty_macro_t *rtty_macro_get(int idx) {
    return idx >= 0 && idx < macro_count ? &macros[idx] : NULL;
}

int rtty_macro_add(const char *name, const char *text) {
    if (macro_count >= RTTY_MACRO_MAX) return -1;
    copy_clean(macros[macro_count].name, sizeof(macros[0].name), name);
    copy_clean(macros[macro_count].text, sizeof(macros[0].text), text);
    return macro_count++;
}

void rtty_macro_set(int idx, const char *name, const char *text) {
    if (idx < 0 || idx >= macro_count) return;
    if (name) copy_clean(macros[idx].name, sizeof(macros[0].name), name);
    if (text) copy_clean(macros[idx].text, sizeof(macros[0].text), text);
}

void rtty_macro_delete(int idx) {
    if (idx < 0 || idx >= macro_count) return;
    memmove(&macros[idx], &macros[idx + 1], (size_t)(macro_count - idx - 1) * sizeof(macros[0]));
    macro_count--;
}
