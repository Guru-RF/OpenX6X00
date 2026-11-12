/* Host test of ft8_udp_log_record (#248): see the Makefile. argv[1] is BOARD_DATA, where ft8_udp.conf goes. */
#include "ft8_udp.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int         fails, checks;
static const char *data_dir;
static int         rx_fd;
static uint16_t    rx_port;

#define CHECK(c, ...)                                         \
    do {                                                      \
        checks++;                                             \
        if (!(c)) {                                           \
            fails++;                                          \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);       \
            printf(__VA_ARGS__);                              \
            printf("\n");                                     \
        }                                                     \
    } while (0)

static int open_fds(void) {
    DIR *d = opendir("/proc/self/fd");
    int  n = 0;

    while (readdir(d))
        n++;
    closedir(d);
    return n;
}

static void conf(int enabled) {
    char  path[512];
    FILE *f;

    snprintf(path, sizeof(path), "%s/ft8_udp.conf", data_dir);
    f = fopen(path, "w");
    fprintf(f, "enabled=%d\nhost=127.0.0.1\nport=%u\nid=TEST\n", enabled, (unsigned)rx_port);
    fclose(f);
}

/* One datagram within ms, its length; 0 none */
static int rx(uint8_t *buf, size_t size, int ms) {
    struct pollfd p = {.fd = rx_fd, .events = POLLIN};

    if (poll(&p, 1, ms) <= 0)
        return 0;
    return (int)recv(rx_fd, buf, size, 0);
}

/* A reader of Qt's QDataStream, as WSJT-X writes it: big-endian, strings with a 32-bit length */
typedef struct {
    const uint8_t *p;
    int            n, o;
    bool           bad;
} rd_t;

static uint64_t rd_n(rd_t *r, int bytes) {
    uint64_t v = 0;

    if (r->o + bytes > r->n) {
        r->bad = true;
        return 0;
    }
    for (int i = 0; i < bytes; i++)
        v = v << 8 | r->p[r->o++];
    return v;
}

static const char *rd_s(rd_t *r) {
    static char s[4][1200];
    static int  k;
    uint32_t    len = (uint32_t)rd_n(r, 4);
    char       *out = s[k++ % 4];

    if (r->bad || len >= sizeof(s[0]) || r->o + (int)len > r->n) {
        r->bad = true;
        return "";
    }
    memcpy(out, r->p + r->o, len);
    out[len] = '\0';
    r->o += (int)len;
    return out;
}

/* The header, its type */
static uint32_t rd_head(rd_t *r) {
    CHECK(rd_n(r, 4) == 0xadbccbdaU, "magic");
    CHECK(rd_n(r, 4) == 3, "schema");
    uint32_t t = (uint32_t)rd_n(r, 4);
    CHECK(strcmp(rd_s(r), "TEST") == 0, "id");
    return t;
}

/* 2026-11-12 13:45:30 UTC */
#define WHEN     ((time_t)1794491130)
#define JDN      2461357ULL
#define MS       ((13 * 3600 + 45 * 60 + 30) * 1000ULL)
#define RECORD   "<call:6>ON4ABC <qso_date:8>20261112 <time_on:6>134530 <mode:3>SSB <eor>\n"

static void log_one(const char *record) {
    ft8_udp_log_record(WHEN, "ON4ABC", "JO10", 14250000ULL, "SSB", "59", "57", "10", "Jan", "PA3XYZ", "JO21", record);
}

static void check_logged(const uint8_t *buf, int n) {
    rd_t r = {buf, n, 0, false};

    CHECK(rd_head(&r) == 5, "type 5 first");
    CHECK(rd_n(&r, 8) == JDN, "date off");
    CHECK(rd_n(&r, 4) == MS, "time off");
    CHECK(rd_n(&r, 1) == 1, "UTC");
    CHECK(strcmp(rd_s(&r), "ON4ABC") == 0, "dx call");
    CHECK(strcmp(rd_s(&r), "JO10") == 0, "dx grid");
    CHECK(rd_n(&r, 8) == 14250000ULL, "frequency");
    CHECK(strcmp(rd_s(&r), "SSB") == 0, "mode");
    CHECK(strcmp(rd_s(&r), "59") == 0, "rst sent");
    CHECK(strcmp(rd_s(&r), "57") == 0, "rst rcvd");
    CHECK(strcmp(rd_s(&r), "10") == 0, "power");
    CHECK(strcmp(rd_s(&r), "") == 0, "comments");
    CHECK(strcmp(rd_s(&r), "Jan") == 0, "name");
    CHECK(rd_n(&r, 8) == JDN && rd_n(&r, 4) == MS && rd_n(&r, 1) == 1, "date/time on");
    CHECK(strcmp(rd_s(&r), "PA3XYZ") == 0, "operator");
    CHECK(strcmp(rd_s(&r), "PA3XYZ") == 0, "my call");
    CHECK(strcmp(rd_s(&r), "JO21") == 0, "my grid");
    for (int i = 0; i < 3; i++)
        CHECK(strcmp(rd_s(&r), "") == 0, "exchange / propagation %d", i);
    CHECK(!r.bad && r.o == n, "type 5: %d of %d bytes read", r.o, n);
}

static void check_adif(const uint8_t *buf, int n) {
    rd_t        r = {buf, n, 0, false};
    const char *want = "host ADIF export\n<adif_ver:5>3.1.0\n<programid:9>OpenX6X00\n<EOH>\n" RECORD;

    CHECK(rd_head(&r) == 12, "type 12 second");
    CHECK(strcmp(rd_s(&r), want) == 0, "the ADIF text");
    CHECK(!r.bad && r.o == n, "type 12: %d of %d bytes read", r.o, n);
}

int main(int argc, char **argv) {
    uint8_t            buf[2048];
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t          al = sizeof(a);
    int                fds, n;

    data_dir = argc > 1 ? argv[1] : ".";
    rx_fd = socket(AF_INET, SOCK_DGRAM, 0);
    bind(rx_fd, (struct sockaddr *)&a, sizeof(a));
    getsockname(rx_fd, (struct sockaddr *)&a, &al);
    rx_port = ntohs(a.sin_port);

    /* The FT8 window closed: a socket for the QSO, closed again */
    conf(1);
    fds = open_fds();
    log_one(RECORD);
    n = rx(buf, sizeof(buf), 1000);
    CHECK(n > 0, "a QSO Logged");
    if (n > 0)
        check_logged(buf, n);
    n = rx(buf, sizeof(buf), 1000);
    CHECK(n > 0, "a Logged ADIF");
    if (n > 0)
        check_adif(buf, n);
    CHECK(rx(buf, sizeof(buf), 200) == 0, "two datagrams, no more");
    CHECK(open_fds() == fds, "socket closed again: %d fds, %d before", open_fds(), fds);

    /* No record: QSO Logged alone */
    log_one("");
    CHECK(rx(buf, sizeof(buf), 1000) > 0 && rx(buf, sizeof(buf), 200) == 0, "no record: one datagram");
    log_one(NULL);
    CHECK(rx(buf, sizeof(buf), 1000) > 0 && rx(buf, sizeof(buf), 200) == 0, "NULL record: one datagram");

    /* A record too long for a datagram: QSO Logged alone, no cut ADIF */
    {
        static char big[1100];
        memset(big, 'x', sizeof(big) - 1);
        log_one(big);
        n = rx(buf, sizeof(buf), 1000);
        CHECK(n > 0 && buf[11] == 5, "long record: QSO Logged");
        CHECK(rx(buf, sizeof(buf), 200) == 0, "long record: no Logged ADIF");
    }

    /* enabled=0: nothing, no socket */
    conf(0);
    fds = open_fds();
    log_one(RECORD);
    CHECK(rx(buf, sizeof(buf), 300) == 0, "enabled=0: nothing sent");
    CHECK(open_fds() == fds, "enabled=0: no socket left");

    /* The FT8 window open: its socket, kept */
    conf(1);
    fds = open_fds();
    ft8_udp_init();
    int open_fd = open_fds();
    CHECK(open_fd == fds + 1, "the window's socket");
    log_one(RECORD);
    n = rx(buf, sizeof(buf), 1000);
    CHECK(n > 0 && buf[11] == 5, "window open: QSO Logged");
    n = rx(buf, sizeof(buf), 1000);
    CHECK(n > 0 && buf[11] == 12, "window open: Logged ADIF");
    CHECK(open_fds() == open_fd, "window open: its socket kept");
    ft8_udp_deinit();
    CHECK(open_fds() == fds, "window closed: socket closed");

    printf("ft8_udp: %d of %d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
