/* Review of C08 (audit of 26 October): the WebSocket handshake of the TCI server and the browser remote
 * (src/tci_ws.c ws_handshake, included whole) takes a browser's socket, which sends Origin, only from a page at
 * the Host it came to, and only by one of the radio's own names, as the web configurator takes a change. A site
 * that points a name of its own at the radio (DNS rebinding) sends that name as Host and as Origin alike, and was
 * taken: TCI has no login and keys the transmitter. TCI programs send no Origin and are taken by any name.
 * Issue #58 (Tailscale Funnel): a request with tailscaled's Tailscale-Funnel-Request header is reported as one
 * from the internet, with its X-Forwarded-For address; Tailscale's proxy adds headers, so a request of up to
 * 4096 bytes is taken (2048 before).
 *
 *     cc -std=c11 -D_GNU_SOURCE -Isrc ws-handshake.c -o ws-handshake && ./ws-handshake
 *
 * Prints each case; exit status 1 when one is not as it should be. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char status_path[512]; /* x6100-tailscale's status file, a temporary one here */
#define WS_TAILSCALE_STATUS status_path
#include "tci_ws.c"

static int fails, cases;

/* A request with more headers (extra: "Name: value\r\n" lines), taken (1) or refused (-1); what it says of its
 * peer: from the internet over Funnel (funnel 1) or not (0), and the address the radio shows for it */
static void expect_peer(const char *host, const char *origin, const char *extra, int want, int want_funnel,
                        const char *want_addr) {
    static char req[8192];
    char        resp[256];
    ws_peer_t   peer;
    int         n   = snprintf(req, sizeof(req),
                               "GET /ws HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                               "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n%s%s%s%s\r\n",
                               host, origin ? "Origin: " : "", origin ? origin : "", origin ? "\r\n" : "", extra);
    int         got = ws_handshake(req, (size_t)n, resp, sizeof(resp), &peer);
    bool        ok  = got == want && (got != 1 || want_funnel < 0 ||
                                      (peer.funnel == (want_funnel == 1) && !strcmp(peer.addr, want_addr)));
    printf("%s Host %-34s Origin %-38s %s", ok ? "ok  " : "FAIL", host, origin ? origin : "(none)",
           got == 1 ? "taken" : "refused");
    if (got == 1 && want_funnel >= 0)
        printf(", %s%s%s", peer.funnel ? "over Funnel" : "not over Funnel", peer.addr[0] ? " from " : "", peer.addr);
    if (extra[0])
        printf(" (%d bytes)", n);
    printf("\n");
    fails += !ok;
    cases++;
}

static void expect(const char *host, const char *origin, int want) {
    expect_peer(host, origin, "", want, -1, "");
}

int main(void) {
    char        own[200], host[300], origin[320], dir[400];
    const char *tmp = getenv("AUDIT_TMP"); /* run.py's temporary directory */

    snprintf(dir, sizeof(dir), "%s/ws-handshake-XXXXXX", tmp ? tmp : "/tmp");
    if (gethostname(own, sizeof(own)) != 0 || !mkdtemp(dir))
        return 2;
    own[sizeof(own) - 1] = 0;
    for (char *p = own; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    snprintf(status_path, sizeof(status_path), "%s/status", dir);

    puts("TCI programs: no Origin, any name");
    expect("192.168.1.50:50001", NULL, 1);
    expect("evil.example:50001", NULL, 1);
    puts("The radio's own pages, by its names");
    expect("192.168.1.50:50001", "http://192.168.1.50", 1);
    expect("[fe80::1]:50001", "http://[fe80::1]", 1);
    expect("localhost:50001", "http://localhost:8080", 1);
    snprintf(host, sizeof(host), "%s:50001", own);
    snprintf(origin, sizeof(origin), "http://%s", own);
    expect(host, origin, 1);
    snprintf(host, sizeof(host), "%s.local:50001", own);
    snprintf(origin, sizeof(origin), "https://%s.local", own);
    expect(host, origin, 1);
    for (char *p = host; *p; p++)
        *p = (char)toupper((unsigned char)*p);
    snprintf(origin, sizeof(origin), "http://%s.local.", own);
    expect(host, origin, 1);
    expect("x6200-1.tail1234.ts.net:50001", "https://x6200-1.tail1234.ts.net", 1);
    puts("A page of another site");
    expect("192.168.1.50:50001", "http://evil.example", -1);
    expect("[fe80::1]:50001", "http://[fe80::2]", -1);
    expect("192.168.1.50:50001", "null", -1);
    expect("", "http://192.168.1.50", -1);
    puts("A site that points a name of its own at the radio (DNS rebinding): its name as Host and Origin");
    expect("evil.example:50001", "http://evil.example", -1);
    expect("x6100.fritz.box:50001", "http://x6100.fritz.box", -1);
    snprintf(host, sizeof(host), "%s.local.evil.example:50001", own);
    snprintf(origin, sizeof(origin), "http://%s.local.evil.example", own);
    expect(host, origin, -1);
    expect("0x7f.1:50001", "http://0x7f.1", -1);
    puts("MagicDNS's short name: while x6100-tailscale's status file names the radio x6200-1.tail1234.ts.net");
    expect("x6200-1:50001", "http://x6200-1", -1);
    FILE *f = fopen(status_path, "w");
    if (!f)
        return 2;
    fputs("state=running\ntext=Connected\nname=X6200-1.tail1234.ts.net\nip=100.101.102.103\n", f);
    fclose(f);
    expect("x6200-1:50001", "http://x6200-1", 1);
    expect("x6200-2:50001", "http://x6200-2", -1);
    expect("tail1234:50001", "http://tail1234", -1);
    remove(status_path);

    puts("Issue #58: through Tailscale's proxy, from the tailnet (serve) and from the internet (Funnel)");
    const char *name = "x6200.tail1234.ts.net", *page = "https://x6200.tail1234.ts.net";
    expect_peer(name, page, "X-Forwarded-For: 100.101.102.104\r\nX-Forwarded-Proto: https\r\n"
                "Tailscale-User-Login: op@example.com\r\n", 1, 0, "");
    expect_peer(name, page, "Tailscale-Funnel-Request: ?1\r\nX-Forwarded-For: 203.0.113.7\r\n", 1, 1, "203.0.113.7");
    expect_peer(name, page, "tailscale-funnel-request: ?1\r\nX-Forwarded-For: 2001:db8::7, 10.0.0.1\r\n", 1, 1,
                "2001:db8::7");
    expect_peer(name, page, "Tailscale-Funnel-Request: ?1\r\nX-Forwarded-For: <script>\r\n", 1, 1, "");
    expect_peer(name, page, "Tailscale-Funnel-Request: ?1\r\n", 1, 1, "");
    expect_peer("192.168.1.50:50001", NULL, "X-Forwarded-For: 203.0.113.9\r\n", 1, 0, "");
    {
        static char big[5000];
        size_t      k = 0;
        /* a profile picture's address and the rest of a tailnet request: 3 kB, which 2048 bytes refused */
        k += snprintf(big + k, sizeof(big) - k, "Tailscale-User-Profile-Pic: https://lh3.example.com/a/");
        while (k < 3000)
            big[k++] = 'x';
        k += snprintf(big + k, sizeof(big) - k, "\r\nTailscale-Funnel-Request: ?1\r\nX-Forwarded-For: 198.51.100.2\r\n");
        expect_peer(name, page, big, 1, 1, "198.51.100.2");
        while (k < 4200)
            big[k++] = 'y';
        snprintf(big + k, sizeof(big) - k, "\r\n");
        expect_peer(name, page, big, -1, -1, "");
    }
    rmdir(dir);
    printf("%d of %d cases as they should be\n", cases - fails, cases);
    return fails != 0;
}
