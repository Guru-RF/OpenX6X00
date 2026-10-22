// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Xiegu X6100 LVGL GUI - named channels, kept in /mnt/channels.json (channel_list.h).
//
// The file, as cJSON prints it (tabs, one key per line, a newline at the end):
//
//   { "version": 1, "channels": [ { "name": "NAVTEX MF", "frequency": 517000, "mode": "USB-D",
//     "agc": "SLOW", "pre": false, "att": false }, ... ] }
//
// An entry with a missing field, a wrong type, a frequency out of 1..4294967295 Hz or an
// unknown mode or AGC is left out, as the web configurator does. Writes go through a .tmp
// file and a rename, since the web configurator may read the file at any moment.
//
// Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware

#include "channel_list.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "cfg/cfg_api.h"

#define CHANNELS_FILE   "/mnt/channels.json"
#define CHANNELS_TMP    CHANNELS_FILE ".tmp"
#define COUNT(a)        (sizeof(a) / sizeof((a)[0]))

static channel_t        list[CHANNELS_MAX];
static uint16_t         list_len;
static struct timespec  file_time;     /* mtime when this GUI last read or wrote the file */

/* What a new radio starts with: weather and navigation broadcasts worth a listen */
static const channel_t presets[] = {
    { "NAVTEX MF",       517000,   radio_mode_usb_dig, radio_agc_slow },
    { "NAVTEX HF",       4208500,  radio_mode_usb_dig, radio_agc_slow },
    { "SHANNON VOLMET",  3413000,  radio_mode_usb,     radio_agc_fast },
    { "SHANNON VOLMET",  5505000,  radio_mode_usb,     radio_agc_fast },
    { "SHANNON VOLMET",  8957000,  radio_mode_usb,     radio_agc_fast },
    { "SHANNON VOLMET",  13264000, radio_mode_usb,     radio_agc_fast },
    { "RTTY DWD DDK2",   4582200,  radio_mode_usb_dig, radio_agc_fast },
    { "RTTY DWD DDH7",   7645200,  radio_mode_usb_dig, radio_agc_fast },
    { "RTTY DWD DDK9",   10100000, radio_mode_usb_dig, radio_agc_fast },
    { "RTTY DWD DDH9",   11038200, radio_mode_usb_dig, radio_agc_fast },
    { "RTTY DWD DDH8",   14466500, radio_mode_usb_dig, radio_agc_fast },
    { "WEFAX DWD DDH3",  3853500,  radio_mode_usb_dig, radio_agc_slow },
    { "WEFAX DWD DDK3",  7878470,  radio_mode_usb_dig, radio_agc_slow },
    { "WEFAX DWD DDK6",  13881000, radio_mode_usb_dig, radio_agc_slow },
    { "WEFAX GYA",       11085000, radio_mode_usb_dig, radio_agc_slow },
    { "WEFAX GYA",       8038500,  radio_mode_usb_dig, radio_agc_slow },
    { "WEFAX GYA",       4608500,  radio_mode_usb_dig, radio_agc_slow },
    { "WEFAX GYA",       2617000,  radio_mode_usb_dig, radio_agc_slow },
};

/* The first name of each table is also what an unknown value is written as */
static const struct { radio_mode_t mode; const char *name; } mode_names[] = {
    { radio_mode_usb, "USB" },      { radio_mode_lsb, "LSB" },
    { radio_mode_lsb_dig, "LSB-D" }, { radio_mode_usb_dig, "USB-D" },
    { radio_mode_cw, "CW" },        { radio_mode_cwr, "CWR" },
    { radio_mode_am, "AM" },        { radio_mode_nfm, "NFM" },
};

static const struct { radio_agc_t agc; const char *name; } agc_names[] = {
    { radio_agc_auto, "AUTO" }, { radio_agc_off, "OFF" },
    { radio_agc_slow, "SLOW" }, { radio_agc_fast, "FAST" },
};

const char *channels_mode_name(radio_mode_t mode) {
    for (size_t i = 0; i < COUNT(mode_names); i++)
        if (mode_names[i].mode == mode) return mode_names[i].name;
    return mode_names[0].name;
}

bool channels_mode_parse(const char *value, radio_mode_t *mode) {
    if (!value || !mode) return false;
    for (size_t i = 0; i < COUNT(mode_names); i++)
        if (!strcmp(mode_names[i].name, value)) {
            *mode = mode_names[i].mode;
            return true;
        }
    return false;
}

const char *channels_agc_name(radio_agc_t agc) {
    for (size_t i = 0; i < COUNT(agc_names); i++)
        if (agc_names[i].agc == agc) return agc_names[i].name;
    return agc_names[0].name;
}

bool channels_agc_parse(const char *value, radio_agc_t *agc) {
    if (!value || !agc) return false;
    for (size_t i = 0; i < COUNT(agc_names); i++)
        if (!strcmp(agc_names[i].name, value)) {
            *agc = agc_names[i].agc;
            return true;
        }
    return false;
}

static bool file_mtime(struct timespec *t) {
    struct stat st;

    if (stat(CHANNELS_FILE, &st) != 0) return false;
    *t = st.st_mtim;
    return true;
}

/* One entry of the file, false when it has to be left out */
static bool parse_entry(const cJSON *it, channel_t *c) {
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it, "name"));
    const cJSON *freq = cJSON_GetObjectItemCaseSensitive(it, "frequency");
    const cJSON *pre = cJSON_GetObjectItemCaseSensitive(it, "pre");
    const cJSON *att = cJSON_GetObjectItemCaseSensitive(it, "att");

    if (!name || !cJSON_IsNumber(freq) || !cJSON_IsBool(pre) || !cJSON_IsBool(att))
        return false;
    if (freq->valuedouble <= 0 || freq->valuedouble > UINT32_MAX)
        return false;
    if (!channels_mode_parse(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it, "mode")), &c->mode) ||
        !channels_agc_parse(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it, "agc")), &c->agc))
        return false;
    snprintf(c->name, sizeof(c->name), "%s", name);
    c->freq = (uint32_t)freq->valuedouble;
    c->pre = cJSON_IsTrue(pre);
    c->att = cJSON_IsTrue(att);
    return true;
}

static char *read_all(FILE *f) {
    if (fseek(f, 0, SEEK_END) != 0) return NULL;

    long size = ftell(f);

    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) return NULL;

    char *buf = malloc(size + 1);

    if (buf && fread(buf, 1, size, f) != (size_t)size) {
        free(buf);
        return NULL;
    }
    if (buf) buf[size] = '\0';
    return buf;
}

void channels_reload_if_changed(void) {
    struct timespec t;

    if (file_mtime(&t) && (t.tv_sec != file_time.tv_sec || t.tv_nsec != file_time.tv_nsec))
        channels_load();
}

bool channels_load(void) {
    FILE *f = fopen(CHANNELS_FILE, "rb");

    if (!f) {
        memcpy(list, presets, sizeof(presets));
        list_len = COUNT(presets);
        return channels_save();     /* the presets, as the file to start from */
    }

    /* Taken before reading: a write that lands meanwhile is picked up by the next poll */
    file_mtime(&file_time);

    char *text = read_all(f);

    fclose(f);
    if (!text) return false;

    cJSON *json = cJSON_Parse(text);

    free(text);

    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(json, "channels");

    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(json);
        return false;
    }

    const cJSON *it;

    list_len = 0;
    cJSON_ArrayForEach(it, arr) {
        if (list_len == CHANNELS_MAX) break;
        if (cJSON_IsObject(it) && parse_entry(it, &list[list_len])) list_len++;
    }
    cJSON_Delete(json);
    return true;
}

static bool write_file(const char *text) {
    FILE *f = fopen(CHANNELS_TMP, "w");

    if (!f) return false;

    bool ok = fputs(text, f) != EOF && fputc('\n', f) != EOF;

    ok = fclose(f) == 0 && ok;
    if (ok && rename(CHANNELS_TMP, CHANNELS_FILE) == 0) {
        file_mtime(&file_time);
        return true;
    }
    remove(CHANNELS_TMP);
    return false;
}

bool channels_save(void) {
    cJSON *json = cJSON_CreateObject();
    bool ok = cJSON_AddNumberToObject(json, "version", 1) != NULL;
    cJSON *arr = cJSON_AddArrayToObject(json, "channels");

    ok = ok && arr;

    for (uint16_t i = 0; ok && i < list_len; i++) {
        const channel_t *c = &list[i];
        cJSON *o = cJSON_CreateObject();

        ok = o && cJSON_AddItemToArray(arr, o);
        if (!ok) {
            cJSON_Delete(o);
            break;
        }
        cJSON_AddStringToObject(o, "name", c->name);
        cJSON_AddNumberToObject(o, "frequency", c->freq);
        cJSON_AddStringToObject(o, "mode", channels_mode_name(c->mode));
        cJSON_AddStringToObject(o, "agc", channels_agc_name(c->agc));
        cJSON_AddBoolToObject(o, "pre", c->pre);
        cJSON_AddBoolToObject(o, "att", c->att);
    }

    char *text = ok ? cJSON_Print(json) : NULL;

    cJSON_Delete(json);
    ok = text && write_file(text);
    cJSON_free(text);
    return ok;
}

bool channels_delete(uint16_t i) {
    if (i >= list_len) return false;

    channel_t gone = list[i];
    size_t tail = (list_len - i - 1) * sizeof(channel_t);

    memmove(&list[i], &list[i + 1], tail);
    list_len--;
    if (channels_save()) return true;

    memmove(&list[i + 1], &list[i], tail);
    list[i] = gone;
    list_len++;
    return false;
}

bool channels_set_name(uint16_t i, const char *name) {
    if (i >= list_len || !name) return false;

    char old[CHANNEL_NAME_MAX];

    memcpy(old, list[i].name, sizeof(old));
    snprintf(list[i].name, sizeof(list[i].name), "%s", name);
    if (channels_save()) return true;

    memcpy(list[i].name, old, sizeof(old));
    return false;
}

bool channels_add_current(void) {
    if (list_len == CHANNELS_MAX) return false;

    channel_t *c = &list[list_len];

    memset(c, 0, sizeof(*c));
    c->freq = cparam_i_get(cfg_fg_freq);
    c->mode = cparam_i_get(cfg_cur_mode);
    c->agc = cparam_i_get(cfg_cur_agc);
    c->pre = cparam_i_get(cfg_cur_pre) != 0;
    c->att = cparam_i_get(cfg_cur_att) != 0;

    list_len++;
    if (channels_save()) return true;
    list_len--;
    return false;
}

bool channels_move(uint16_t from, uint16_t to) {
    if (from >= list_len || to >= list_len) return false;

    channel_t c = list[from];

    if (from < to)
        memmove(&list[from], &list[from + 1], (to - from) * sizeof(channel_t));
    else
        memmove(&list[to + 1], &list[to], (from - to) * sizeof(channel_t));
    list[to] = c;
    return true;
}

uint16_t channels_count(void) {
    return list_len;
}

const channel_t *channels_get(uint16_t i) {
    return i < list_len ? &list[i] : NULL;
}
