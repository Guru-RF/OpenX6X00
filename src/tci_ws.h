/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WebSocket framing for the TCI server (tci_server.c): the HTTP upgrade
 *  handshake (RFC 6455, with its own SHA-1 and base64), the client's masked frames read
 *  back, the server's frames written out. Nothing in here touches a socket. Also the SHA-256
 *  the browser remote keeps its session tokens as (remote_server.c).
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

#define WS_REQUEST_MAX 4096   /* the longest upgrade request taken: Tailscale's proxy adds headers of its own */

/* Where an upgrade request came from, as its headers say */
typedef struct {
    bool funnel;            /* Tailscale-Funnel-Request: from the internet, through Tailscale Funnel (#58) */
    char addr[48];          /* with it, the browser's address (X-Forwarded-For, which tailscaled sets), "" none */
} ws_peer_t;

/* An HTTP request (the bytes read so far, len of them): complete when the blank line is in.
 * Returns 1 and the 101 response in resp when it is a WebSocket upgrade, 0 when the request
 * is not complete yet, -1 when it is complete but not an upgrade we take. A browser's (it
 * sends Origin) is taken from the radio's own pages only, opened by one of its own names: an
 * address, localhost, the hostname, <hostname>.local, <hostname>.<domain> for a domain of its
 * network, a name added on the web configurator's Network page, *.ts.net or MagicDNS's short
 * name (#105); on the radio not from port 8080, Pat's web mailbox (WS_REFUSED_ORIGIN_PORT in tci_ws.c).
 * TCI's and the browser remote's sockets both come here. peer (NULL: not asked) says whether it
 * came over Tailscale Funnel: tailscaled sets the header on every request from the internet and
 * drops one the browser sent, so only the internet's requests have it (a client on the LAN that
 * sends it is taken for one from the internet, which only holds it back). The caller decides
 * what such a client may do */
int ws_handshake(const char *req, size_t len, char *resp, size_t resp_max, ws_peer_t *peer);

/* One frame from the start of buf: returns the bytes it took (header and payload), 0 when
 * the buffer holds no complete frame yet, -1 on a frame we refuse (unmasked, reserved bits,
 * longer than max_payload). The payload is unmasked in place. */
int ws_frame_parse(uint8_t *buf, size_t len, size_t max_payload, ws_frame_t *out);

/* The header of a server frame (fin set, no mask) for payload_len bytes: its length */
size_t ws_frame_head(uint8_t *head, uint8_t opcode, size_t payload_len);

/* 20 bytes of SHA-1 of data; base64 of n bytes into out (NUL terminated, 4 * ceil(n / 3) + 1) */
void   ws_sha1(const uint8_t *data, size_t len, uint8_t digest[20]);
size_t ws_base64(const uint8_t *in, size_t n, char *out, size_t out_max);

/* 32 bytes of SHA-256 of data; n bytes compared in a time that does not depend on where they differ */
void ws_sha256(const uint8_t *data, size_t len, uint8_t digest[32]);
bool ws_same(const void *a, const void *b, size_t n);

#ifdef __cplusplus
}
#endif
