/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WebSocket framing for the TCI server (tci_ws.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "tci_ws.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* x6100-tailscale's status file: its name= line is the radio's MagicDNS name while it is on a tailnet */
#ifndef WS_TAILSCALE_STATUS
#define WS_TAILSCALE_STATUS "/run/tailscale/status"
#endif

/* ---- SHA-1 (FIPS 180-4), for the handshake only ------------------------------------ */

static uint32_t rol(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

static void sha1_block(uint32_t h[5], const uint8_t *p) {
    uint32_t w[80];
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];

    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      f = (b & c) | (~b & d),           k = 0x5A827999;
        else if (i < 40) f = b ^ c ^ d,                    k = 0x6ED9EBA1;
        else if (i < 60) f = (b & c) | (b & d) | (c & d),  k = 0x8F1BBCDC;
        else             f = b ^ c ^ d,                    k = 0xCA62C1D6;
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
}

void ws_sha1(const uint8_t *data, size_t len, uint8_t digest[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t  tail[128];
    size_t   i, rest;

    for (i = 0; i + 64 <= len; i += 64)
        sha1_block(h, data + i);
    rest = len - i;
    memset(tail, 0, sizeof(tail));
    memcpy(tail, data + i, rest);
    tail[rest] = 0x80;
    size_t   tl   = rest < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int k = 0; k < 8; k++)
        tail[tl - 1 - k] = (uint8_t)(bits >> (8 * k));
    sha1_block(h, tail);
    if (tl == 128)
        sha1_block(h, tail + 64);
    for (int k = 0; k < 5; k++) {
        digest[k * 4]     = (uint8_t)(h[k] >> 24);
        digest[k * 4 + 1] = (uint8_t)(h[k] >> 16);
        digest[k * 4 + 2] = (uint8_t)(h[k] >> 8);
        digest[k * 4 + 3] = (uint8_t)h[k];
    }
}

size_t ws_base64(const uint8_t *in, size_t n, char *out, size_t out_max) {
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t            o     = 0;

    if (out_max < 4 * ((n + 2) / 3) + 1)
        return 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = tab[v >> 18 & 63];
        out[o++] = tab[v >> 12 & 63];
        out[o++] = i + 1 < n ? tab[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? tab[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

/* ---- Handshake ----------------------------------------------------------------------- */

/* The value of a header, trimmed, NULL when absent (names compared without case) */
static const char *header(const char *req, const char *name, size_t *vlen) {
    size_t      nl = strlen(name);
    const char *p  = req;

    while ((p = strchr(p, '\n'))) {
        p++;
        if (strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            const char *e = v;
            while (*e && *e != '\r' && *e != '\n') e++;
            while (e > v && (e[-1] == ' ' || e[-1] == '\t')) e--;
            *vlen = e - v;
            return v;
        }
    }
    return NULL;
}

/* The host of a Host header or of an Origin's authority (n bytes): without the port and an IPv6 address's
 * brackets, in lower case, without a trailing dot. False when there is none or it does not fit */
static bool host_name(const char *h, size_t n, char *out, size_t out_max) {
    const char *end = h + n;

    if (n && h[0] == '[') { /* [fe80::1]:50001 */
        const char *rb = memchr(h, ']', n);
        if (!rb)
            return false;
        end = rb;
        h++;
    } else {
        const char *c = memchr(h, ':', n);
        if (c)
            end = c;
    }
    size_t l = end - h;
    if (l && h[l - 1] == '.')
        l--;
    if (!l || l >= out_max)
        return false;
    for (size_t i = 0; i < l; i++)
        out[i] = (char)tolower((unsigned char)h[i]);
    out[l] = 0;
    return true;
}

/* The radio's MagicDNS short name (the first part of name= in the status file), lower case; "" when none */
static void tailnet_name(char *out, size_t out_max) {
    char  line[256];
    FILE *f = fopen(WS_TAILSCALE_STATUS, "r");

    out[0] = 0;
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "name=", 5) == 0) {
            size_t l = strcspn(line + 5, ".\r\n");
            if (l < out_max) {
                for (size_t i = 0; i < l; i++)
                    out[i] = (char)tolower((unsigned char)line[5 + i]);
                out[l] = 0;
            }
            break;
        }
    }
    fclose(f);
}

/* A name of the radio's own (host_name's form): an IP address, localhost, the hostname, <hostname>.local, a
 * Tailscale name (*.ts.net) or MagicDNS's short name, the names the web configurator takes (local_host_ok in
 * webserver/.../apps.py) and Pat's mailbox too (tools/pat) */
static bool host_local(const char *name) {
    struct in6_addr a6;
    struct in_addr  a4;
    char            own[256], tailnet[64];
    size_t          n = strlen(name), o;

    if (inet_pton(AF_INET, name, &a4) == 1 || inet_pton(AF_INET6, name, &a6) == 1)
        return true;
    if (strcmp(name, "localhost") == 0 || (n > 7 && strcmp(name + n - 7, ".ts.net") == 0))
        return true;
    if (gethostname(own, sizeof(own)) == 0) {
        own[sizeof(own) - 1] = 0;
        for (o = 0; own[o]; o++)
            own[o] = (char)tolower((unsigned char)own[o]);
        if (o && strcmp(name, own) == 0)
            return true;
        if (o && n == o + 6 && strncmp(name, own, o) == 0 && strcmp(name + o, ".local") == 0)
            return true;
    }
    tailnet_name(tailnet, sizeof(tailnet));
    return tailnet[0] && strcmp(name, tailnet) == 0;
}

int ws_handshake(const char *req, size_t len, char *resp, size_t resp_max) {
    char line[2048];

    if (len >= sizeof(line))
        return -1;
    memcpy(line, req, len);
    line[len] = 0;
    if (!strstr(line, "\r\n\r\n") && !strstr(line, "\n\n"))
        return 0;
    if (strncmp(line, "GET ", 4) != 0)
        return -1;
    size_t      kl, ul, ol, hl;
    const char *key = header(line, "Sec-WebSocket-Key", &kl);
    const char *upg = header(line, "Upgrade", &ul);
    if (!key || !kl || kl > 64 || !upg || strncasecmp(upg, "websocket", 9) != 0)
        return -1;

    /* A browser sends Origin, a TCI program does not: a web page from elsewhere (an ad, a
     * phishing page) could otherwise key the radio from any browser on the LAN. Only the
     * radio's own pages (an Origin at the Host the request came to) may, and only by one of
     * the radio's own names: a site that points a name of its own at the radio (DNS
     * rebinding) sends that name as Host and as Origin alike, and TCI has no login (review of
     * audit C08: the web configurator takes changes by those names only, now these sockets too) */
    const char *org  = header(line, "Origin", &ol);
    const char *host = header(line, "Host", &hl);
    if (org && ol) {
        const char *o = memchr(org, ':', ol);                /* past "http:" or "https:" */
        if (!o || ol < (size_t)(o - org) + 3 || o[1] != '/' || o[2] != '/')
            return -1;
        o += 3;
        /* the hosts without their ports: the page comes from port 80, the socket is on 50001 */
        char oh[256], hh[256];
        if (!host || !host_name(o, ol - (o - org), oh, sizeof(oh)) || !host_name(host, hl, hh, sizeof(hh)) ||
            strcmp(oh, hh) != 0 || !host_local(hh))
            return -1;
    }

    /* accept = base64(sha1(key + the RFC's GUID)) */
    char    cat[64 + 36 + 1];
    uint8_t digest[20];
    char    accept[32];
    memcpy(cat, key, kl);
    memcpy(cat + kl, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36);
    ws_sha1((const uint8_t *)cat, kl + 36, digest);
    ws_base64(digest, 20, accept, sizeof(accept));

    int n = snprintf(resp, resp_max,
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n",
                     accept);
    return n > 0 && (size_t)n < resp_max ? 1 : -1;
}

/* ---- Frames -------------------------------------------------------------------------- */

int ws_frame_parse(uint8_t *buf, size_t len, size_t max_payload, ws_frame_t *out) {
    if (len < 2)
        return 0;
    uint8_t b0 = buf[0], b1 = buf[1];
    if (b0 & 0x70)              /* reserved bits: no extension was negotiated */
        return -1;
    if (!(b1 & 0x80))           /* a client must mask */
        return -1;
    size_t   plen = b1 & 0x7f;
    size_t   pos  = 2;
    if (plen == 126) {
        if (len < 4) return 0;
        plen = (size_t)buf[2] << 8 | buf[3];
        pos  = 4;
    } else if (plen == 127) {
        if (len < 10) return 0;
        for (int i = 0; i < 4; i++)         /* over 4 GB: refused below anyway */
            if (buf[2 + i]) return -1;
        plen = (size_t)buf[6] << 24 | (size_t)buf[7] << 16 | (size_t)buf[8] << 8 | buf[9];
        pos  = 10;
    }
    if (plen > max_payload)
        return -1;
    if (len < pos + 4 + plen)
        return 0;
    const uint8_t *mask = buf + pos;
    uint8_t       *p    = buf + pos + 4;
    for (size_t i = 0; i < plen; i++)
        p[i] ^= mask[i & 3];
    out->opcode  = b0 & 0x0f;
    out->fin     = (b0 & 0x80) != 0;
    out->payload = p;
    out->len     = plen;
    return (int)(pos + 4 + plen);
}

size_t ws_frame_head(uint8_t *head, uint8_t opcode, size_t payload_len) {
    head[0] = 0x80 | (opcode & 0x0f);
    if (payload_len < 126) {
        head[1] = (uint8_t)payload_len;
        return 2;
    }
    if (payload_len < 65536) {
        head[1] = 126;
        head[2] = (uint8_t)(payload_len >> 8);
        head[3] = (uint8_t)payload_len;
        return 4;
    }
    head[1] = 127;
    for (int i = 0; i < 8; i++)
        head[2 + i] = (uint8_t)((uint64_t)payload_len >> (8 * (7 - i)));
    return 10;
}
