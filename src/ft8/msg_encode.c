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

/* A report, "-07", "+12", or an R-report, "R-07" */
static bool is_report(const char *s) {
    if (*s == 'R') s++;
    if (*s != '+' && *s != '-') return false;
    size_t n = strlen(++s);
    return n >= 1 && n <= 2 && strspn(s, "0123456789") == n;
}

/* Free text as WSJT-X packs it, right-aligned in its 13 characters: the same bits on the air. ft8_lib pads it on the
 * right; a decoder trims either */
static ftx_message_rc_t encode_free(ftx_message_t *msg, const char *text) {
    char padded[14];

    if (strlen(text) > 13) return FTX_MESSAGE_RC_ERROR_TYPE;
    snprintf(padded, sizeof(padded), "%13s", text);
    ftx_message_init(msg);
    return ftx_message_encode_free(msg, padded);
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
    /* One call and a report or an end, "26AT101 -07", "26AT101 R-07", "26AT101 RR73", "26AT101 73": WSJT-CB's messages
     * between two CB calls, which FT8 cannot carry both of with a report. WSJT-X packs any two words that are not two
     * calls as free text; ft8_lib took "RR73" and "RRR" for a call and sent a hash of them ("<26AT101> <RR73>") */
    if (n == 2 && (is_report(de) || is_short_ack(de))) return encode_free(msg, text);
    ftx_message_init(msg);
    ftx_message_rc_t rc = ftx_message_encode(msg, hash_if, text);
    if (rc == FTX_MESSAGE_RC_OK && ftx_message_get_type(msg) == FTX_MESSAGE_TYPE_FREE_TEXT) return encode_free(msg, text);
    return rc;
}
