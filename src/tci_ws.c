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
/* The domains the radio's network gives it: the search and domain lines NetworkManager writes for the connections
 * that are up (the radio's /etc/resolv.conf is a link to /run/NetworkManager/resolv.conf) */
#ifndef WS_RESOLV_CONF
#define WS_RESOLV_CONF "/etc/resolv.conf"
#endif
/* The names its owner added on the web configurator's Network page, one per line (#105) */
#ifndef WS_OWN_NAMES
#define WS_OWN_NAMES "/mnt/own_names.conf"
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

/* ---- SHA-256 (FIPS 180-4), for the browser remote's session tokens --------------------- */

static uint32_t ror(uint32_t v, int n) {
    return (v >> n) | (v << (32 - n));
}

static void sha256_block(uint32_t h[8], const uint8_t *p) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    uint32_t w[64];
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];

    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

void ws_sha256(const uint8_t *data, size_t len, uint8_t digest[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t  tail[128];
    size_t   i, rest;

    for (i = 0; i + 64 <= len; i += 64)
        sha256_block(h, data + i);
    rest = len - i;
    memset(tail, 0, sizeof(tail));
    memcpy(tail, data + i, rest);
    tail[rest] = 0x80;
    size_t   tl   = rest < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int k = 0; k < 8; k++)
        tail[tl - 1 - k] = (uint8_t)(bits >> (8 * k));
    sha256_block(h, tail);
    if (tl == 128)
        sha256_block(h, tail + 64);
    for (int k = 0; k < 8; k++) {
        digest[k * 4]     = (uint8_t)(h[k] >> 24);
        digest[k * 4 + 1] = (uint8_t)(h[k] >> 16);
        digest[k * 4 + 2] = (uint8_t)(h[k] >> 8);
        digest[k * 4 + 3] = (uint8_t)h[k];
    }
}

bool ws_same(const void *a, const void *b, size_t n) {
    const volatile uint8_t *x = a, *y = b;
    uint8_t                 d = 0;

    for (size_t i = 0; i < n; i++)
        d |= x[i] ^ y[i];
    return d == 0;
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

/* A DNS name in lower case: labels of letters, digits and '-' (not first or last), the last one with a letter */
static bool dns_name(const char *d) {
    size_t label = 0;
    bool   letter = false;

    if (!*d || strlen(d) > 253)
        return false;
    for (const char *p = d;; p++) {
        if (*p == '.' || !*p) {
            if (!label || label > 63 || p[-1] == '-')
                return false;
            if (!*p)
                return letter;
            label = 0, letter = false;
        } else if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-') {
            if (!label && *p == '-')
                return false;
            letter |= *p >= 'a' && *p <= 'z';
            label++;
        } else {
            return false;
        }
    }
}

/* A domain of the radio's network that names it: no public suffix on its own, under which anyone may register a
 * name (x6100.nl could be anyone's). A single name only when routers keep it out of the public DNS (lan, home);
 * no country's second level (co.uk, com.au). As the web configurator's network_domains (webserver/.../apps.py) */
static bool domain_private(const char *d) {
    static const char *const single[]   = {"lan", "home", "localdomain", "internal", "intranet", "corp", "private"};
    static const char *const registry[] = {"ac", "co",  "com", "edu", "go", "gob", "gouv", "gov", "gv",
                                           "ltd", "mil", "ne", "net", "nom", "or", "org", "plc", "sch"};
    const char *dot = strchr(d, '.');

    if (!dot) {
        for (size_t i = 0; i < sizeof(single) / sizeof(single[0]); i++)
            if (strcmp(d, single[i]) == 0)
                return true;
        return false;
    }
    if (!dns_name(d))
        return false;
    if (strchr(dot + 1, '.') || strlen(dot + 1) != 2)
        return true;
    for (size_t i = 0; i < sizeof(registry) / sizeof(registry[0]); i++)
        if (strlen(registry[i]) == (size_t)(dot - d) && strncmp(d, registry[i], dot - d) == 0)
            return false;
    return true;
}

#define WS_NAMES_FILE_MAX 4096   /* the most read of resolv.conf and own_names.conf, in whole lines (as apps.py) */

/* A small file's whole lines into buf (WS_NAMES_FILE_MAX + 1 bytes): at most WS_NAMES_FILE_MAX bytes, a line cut
 * there left out, so no word of it is taken cut short. False when there is no file */
static bool read_lines(const char *path, char *buf) {
    FILE  *f = fopen(path, "r");
    size_t n;

    if (!f)
        return false;
    n = fread(buf, 1, WS_NAMES_FILE_MAX, f);
    if (n == WS_NAMES_FILE_MAX && fgetc(f) != EOF) {
        while (n && buf[n - 1] != '\n')
            n--;
    }
    fclose(f);
    buf[n] = 0;
    return true;
}

/* The word in lower case, without a trailing dot: a domain or a name as resolv.conf and own_names.conf have them */
static void lower_word(char *w) {
    size_t l = 0;

    for (; w[l]; l++)
        w[l] = (char)tolower((unsigned char)w[l]);
    if (l && w[l - 1] == '.')
        w[l - 1] = 0;
}

/* name is <own>.<domain> for a domain the radio's network gives it (resolv.conf's search and domain lines) */
static bool network_name(const char *name, const char *own, size_t o) {
    char buf[WS_NAMES_FILE_MAX + 1], *lines = NULL, *line;

    if (strncmp(name, own, o) != 0 || name[o] != '.' || !read_lines(WS_RESOLV_CONF, buf))
        return false;
    for (line = strtok_r(buf, "\n", &lines); line; line = strtok_r(NULL, "\n", &lines)) {
        char *words = NULL, *w = strtok_r(line, " \t\r", &words);
        if (!w || (strcmp(w, "search") != 0 && strcmp(w, "domain") != 0))
            continue;
        while ((w = strtok_r(NULL, " \t\r", &words))) {
            lower_word(w);
            if (domain_private(w) && strcmp(name + o + 1, w) == 0)
                return true;
        }
    }
    return false;
}

/* name is one the owner added on the Network page (own_names.conf: a name, the first word, on each line) */
static bool listed_name(const char *name) {
    char buf[WS_NAMES_FILE_MAX + 1], *lines = NULL, *line;

    if (!read_lines(WS_OWN_NAMES, buf))
        return false;
    for (line = strtok_r(buf, "\n", &lines); line; line = strtok_r(NULL, "\n", &lines)) {
        char *words = NULL, *w = strtok_r(line, " \t\r", &words);
        if (w && *w != '#') {
            lower_word(w);
            if (strcmp(name, w) == 0)
                return true;
        }
    }
    return false;
}

/* A name of the radio's own (host_name's form): an IP address, localhost, the hostname, <hostname>.local,
 * <hostname>.<domain> for a domain its network gives it, a name its owner added on the Network page, a Tailscale
 * name (*.ts.net) or MagicDNS's short name, the names the web configurator takes (local_host_ok in
 * webserver/.../apps.py) and Pat's mailbox too (tools/pat). The files are read at each browser's handshake: a few
 * hundred bytes, and a page opens a socket or two */
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
        if (o && network_name(name, own, o))
            return true;
    }
    if (listed_name(name))
        return true;
    tailnet_name(tailnet, sizeof(tailnet));
    return tailnet[0] && strcmp(name, tailnet) == 0;
}

/* X-Forwarded-For's first address (the browser's, as tailscaled saw it), checked to be one; "" when none */
static void forwarded_for(const char *req, char *out, size_t out_max) {
    struct in6_addr a6;
    struct in_addr  a4;
    char            a[64];
    size_t          vl, l = 0;
    const char     *v = header(req, "X-Forwarded-For", &vl);

    out[0] = 0;
    if (!v)
        return;
    while (l < vl && v[l] != ',' && v[l] != ' ')
        l++;
    if (!l || l >= sizeof(a) || l >= out_max)
        return;
    memcpy(a, v, l);
    a[l] = 0;
    if (inet_pton(AF_INET, a, &a4) == 1 || inet_pton(AF_INET6, a, &a6) == 1)
        memcpy(out, a, l + 1);
}

int ws_handshake(const char *req, size_t len, char *resp, size_t resp_max, ws_peer_t *peer) {
    char line[WS_REQUEST_MAX];

    if (peer)
        *peer = (ws_peer_t){0};
    /* 2048 bytes were too few behind Tailscale's proxy: its X-Forwarded-*, and on the tailnet its
     * Tailscale-User-* with the address of a profile picture (#58) */
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
    /* A Funnel name is a *.ts.net name, and passes the check above: only this header tells the
     * internet's requests from the tailnet's */
    if (peer) {
        size_t fl;
        peer->funnel = header(line, "Tailscale-Funnel-Request", &fl) != NULL;
        if (peer->funnel)
            forwarded_for(line, peer->addr, sizeof(peer->addr));
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
