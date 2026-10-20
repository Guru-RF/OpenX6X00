/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WebSocket framing for the TCI server (tci_server.c): the HTTP upgrade
 *  handshake (RFC 6455, with its own SHA-1 and base64), the client's masked frames read
 *  back, the server's frames written out. Nothing in here touches a socket.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    WS_OP_CONT   = 0x0,
    WS_OP_TEXT   = 0x1,
    WS_OP_BINARY = 0x2,
    WS_OP_CLOSE  = 0x8,
    WS_OP_PING   = 0x9,
    WS_OP_PONG   = 0xA,
};

#define WS_HEAD_MAX 10   /* longest frame header the server writes (no mask) */

typedef struct {
    uint8_t  opcode;
    bool     fin;
    uint8_t *payload;   /* inside the buffer given to ws_frame_parse, unmasked in place */
    size_t   len;
} ws_frame_t;

/* An HTTP request (the bytes read so far, len of them): complete when the blank line is in.
 * Returns 1 and the 101 response in resp when it is a WebSocket upgrade, 0 when the request
 * is not complete yet, -1 when it is complete but not an upgrade we take. */
int ws_handshake(const char *req, size_t len, char *resp, size_t resp_max);

/* One frame from the start of buf: returns the bytes it took (header and payload), 0 when
 * the buffer holds no complete frame yet, -1 on a frame we refuse (unmasked, reserved bits,
 * longer than max_payload). The payload is unmasked in place. */
int ws_frame_parse(uint8_t *buf, size_t len, size_t max_payload, ws_frame_t *out);

/* The header of a server frame (fin set, no mask) for payload_len bytes: its length */
size_t ws_frame_head(uint8_t *head, uint8_t opcode, size_t payload_len);

/* 20 bytes of SHA-1 of data; base64 of n bytes into out (NUL terminated, 4 * ceil(n / 3) + 1) */
void   ws_sha1(const uint8_t *data, size_t len, uint8_t digest[20]);
size_t ws_base64(const uint8_t *in, size_t n, char *out, size_t out_max);

#ifdef __cplusplus
}
#endif
