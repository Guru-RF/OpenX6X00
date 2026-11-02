/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - FT8/FT4 message text for any callsign
 *
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100), from its d10ae10
 */

#include "msg_encode.h"

#include <stdio.h>
#include <string.h>

#define FTX_CALL_MAX 11     /* plain text in a type 4 message */

/* text goes through FT8 unchanged: encoded and decoded, the same text; *type what it went as */
static bool round_trip(const char *text, ftx_message_type_t *type) {
    ftx_message_t msg;
    char          out[FTX_MAX_MESSAGE_LENGTH];

    ftx_message_init(&msg);
    if (ftx_message_encode(&msg, NULL, text) != FTX_MESSAGE_RC_OK) return false;
    if (ftx_message_decode(&msg, NULL, out) != FTX_MESSAGE_RC_OK) return false;
    *type = ftx_message_get_type(&msg);
    return strcmp(text, out) == 0;
}

bool ftx_call_is_standard(const char *call) {
    char               text[FTX_MAX_MESSAGE_LENGTH];
    ftx_message_type_t type;

    if (!call || !call[0] || strlen(call) > FTX_CALL_MAX || strchr(call, ' ')) return false;
    snprintf(text, sizeof(text), "CQ %s AA00", call);
    return round_trip(text, &type) && type == FTX_MESSAGE_TYPE_STANDARD;
}

static bool is_short_ack(const char *extra) {
    return !strcmp(extra, "RRR") || !strcmp(extra, "RR73") || !strcmp(extra, "73");
}

ftx_message_rc_t ftx_encode_text(ftx_message_t *msg, ftx_callsign_hash_interface_t *hash_if, const char *text) {
    char   to[16], de[16], extra[16], rest[4];
    int    n   = sscanf(text, "%15s %15s %15s %3s", to, de, extra, rest);
    size_t len = n >= 1 ? strlen(to) : 0;

    if ((n == 2 || (n == 3 && is_short_ack(extra))) && len > 2 && to[0] == '<' && to[len - 1] == '>' &&
        !ftx_call_is_standard(de)) {
        char bare[16];

        /* 73b0db8 and 117022b take the hashed call bare; a later ft8_lib may want it in brackets, as written */
        snprintf(bare, sizeof(bare), "%.*s", (int)(len - 2), to + 1);
        ftx_message_init(msg);
        if (ftx_message_encode_nonstd(msg, hash_if, bare, de, n == 3 ? extra : "") == FTX_MESSAGE_RC_OK)
            return FTX_MESSAGE_RC_OK;
        ftx_message_init(msg);
        if (ftx_message_encode_nonstd(msg, hash_if, to, de, n == 3 ? extra : "") == FTX_MESSAGE_RC_OK)
            return FTX_MESSAGE_RC_OK;
    }
    ftx_message_init(msg);
    return ftx_message_encode(msg, hash_if, text);
}
