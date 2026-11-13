/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/gps.c, the GUI's gpsd client (#237), on its own thread with the radios' libgps (gpsd 3.25), against
 *  a stand-in gpsd on a port of this PC that greets as gpsd does (VERSION, then DEVICES and WATCH to the GUI's WATCH)
 *  and sends TPVs and SKYs as one with a GPS. In order: the GUI connects at the start and has the first TPV's
 *  position; a TPV and its SKY go to the GPS window in one copy, with the flags of both; 5000 TPVs at once are all
 *  read within 2 s, on the one connection, to the GPS window in a copy at most every 100 ms, the last one with the
 *  last position (up to v0.3.10 one line every 100 ms: gpsd's lines backed up until it let the GUI go, a copy for
 *  each); gpsd closes the connection: the GUI
 *  connects again after about a second (v0.3.10 read the closed one on and on, the icon grey until a restart), said
 *  once in the log, and once that reports come again; a line longer than libgps's buffer: the same; a GPS that
 *  reports, its port there, for longer than gpsd's start waits: left alone, nothing in the log; that GPS quiet, its
 *  port still there: the GUI connects again at once and then less and less often, and starts gpsd after 1.6 s and
 *  again 3.2 s later; reports again: as at first; quiet with the port gone: no start of gpsd, connections less and
 *  less often, one line in the log; gpsd gone: tried again after 1 s and then every 2 s, said once; a USB device
 *  plugged in cuts the pause short, and the log says gpsd is there again.
 *  gps.c's times are cut short (Makefile). The test's own SIGPIPE is not ignored: a write of gps.c's to a connection
 *  gpsd has closed would end it.
 */

#define _GNU_SOURCE
#include "build/src/gps.h"
#include "build/src/pubsub_ids.h"
#include "build/src/usb_devices.h"
#include "dialog_gps.h"
#include "events.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PORTFILE    "test.d/dev/ttyACM0"    /* the GPS's port, there or not; the TPVs come from it */
#define RESTARTS    "test.d/restarts"       /* a line each time gps.c starts gpsd */
#define FLOOD       5000

static int fails, checks;

static void check(bool ok, const char *fmt, ...) {
    va_list ap;

    checks++;
    if (!ok) {
        fails++;
    }
    printf("%s ", ok ? "ok  " : "FAIL");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static int64_t now_ms(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void sleep_ms(int ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};

    while (nanosleep(&t, &t) != 0 && errno == EINTR) {
    }
}

/* gps.c's log: into gps_test.log, and kept to be counted */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static char            log_text[1 << 16];
static size_t          log_len;
static FILE           *log_file;

void test_log(const char *level, const char *fmt, ...) {
    char    line[512];
    size_t  n;
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    n = strlen(line);
    pthread_mutex_lock(&log_lock);
    if (log_file) {
        fprintf(log_file, "%lld [%s] %s\n", (long long)now_ms(), level, line);
        fflush(log_file);
    }
    if (log_len + n + 2 < sizeof(log_text)) {
        memcpy(log_text + log_len, line, n);
        log_len += n;
        log_text[log_len++] = '\n';
        log_text[log_len] = '\0';
    }
    pthread_mutex_unlock(&log_lock);
}

static int log_count(const char *what) {
    int n = 0;

    pthread_mutex_lock(&log_lock);
    for (const char *p = log_text; (p = strstr(p, what)); p += strlen(what)) {
        n++;
    }
    pthread_mutex_unlock(&log_lock);
    return n;
}

static int log_lines(void) {
    return log_count("\n");
}

/* The USB messages: gps.c's callback, called by the test as the GUI loop does */
static lv_msg_subscribe_cb_t usb_cb;

void *lv_msg_subscribe(uint32_t msg_id, lv_msg_subscribe_cb_t cb, void *user_data) {
    if (msg_id == MSG_USB_DEVICE_CHANGED) {
        usb_cb = cb;
    }
    return NULL;
}

static void usb_added(void) {
    lv_msg_t m = {.id = MSG_USB_DEVICE_CHANGED, .payload = (const void *)(intptr_t)USB_DEV_ADDED};

    usb_cb(NULL, &m);
}

/* The GPS window: open for part of the run; the copies it gets counted, the last one's latitude and flags kept */
static dialog_t        window;
dialog_t              *dialog_gps = &window;
static pthread_mutex_t window_lock = PTHREAD_MUTEX_INITIALIZER;
static int             window_copies;
static double          window_lat;
static gps_mask_t      window_set;

void event_send(lv_obj_t *obj, int event_code, void *param) {
    struct gps_data_t *g = param;

    if (event_code == EVENT_GPS) {
        pthread_mutex_lock(&window_lock);
        window_copies++;
        window_lat = g->fix.latitude;
        window_set = g->set;
        pthread_mutex_unlock(&window_lock);
    }
    free(param);
}

static int copies(double *lat, gps_mask_t *set) {
    int n;

    pthread_mutex_lock(&window_lock);
    n = window_copies;
    if (lat) {
        *lat = window_lat;
    }
    if (set) {
        *set = window_set;
    }
    pthread_mutex_unlock(&window_lock);
    return n;
}

/* gps.c's tries to connect: when, and whether gpsd was there */
int __real_gps_open(const char *host, const char *port, struct gps_data_t *data);

static pthread_mutex_t try_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t         try_at[1024];
static bool            try_ok[1024];
static int             tries;

int __wrap_gps_open(const char *host, const char *port, struct gps_data_t *data) {
    int r = __real_gps_open(host, port, data);

    pthread_mutex_lock(&try_lock);
    if (tries < 1024) {
        try_at[tries] = now_ms();
        try_ok[tries] = r == 0;
        tries++;
    }
    pthread_mutex_unlock(&try_lock);
    return r;
}

static int tries_now(void) {
    int n;

    pthread_mutex_lock(&try_lock);
    n = tries;
    pthread_mutex_unlock(&try_lock);
    return n;
}

/* The stand-in gpsd: its port, the GUI's connection (the last one taken), when each was taken */
static int     listener = -1;
static int     conn = -1;
static int64_t taken_at[256];
static int     taken;

static void listen_on(void) {
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(TEST_PORT)};
    int                one = 1;

    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(listener, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(listener, 8) != 0) {
        perror("the stand-in gpsd's port");
        exit(2);
    }
}

static void listen_off(void) {
    close(listener);
    listener = -1;
}

static bool put_raw(const char *buf, size_t n) {
    while (n > 0) {
        ssize_t k = send(conn, buf, n, MSG_NOSIGNAL);

        if (k <= 0) {
            return false;
        }
        buf += k;
        n -= k;
    }
    return true;
}

static bool put(const char *fmt, ...) {
    char    buf[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return put_raw(buf, strlen(buf));
}

/* The GUI's next connection within ms, greeted as gpsd greets; the one before closed. False: none came */
static bool take(int ms) {
    struct pollfd  p = {listener, POLLIN, 0};
    struct timeval tv = {5, 0};     /* a GUI that stops reading fails the send, the test goes on */
    char           buf[512];
    int            c;

    if (poll(&p, 1, ms) <= 0 || (c = accept4(listener, NULL, NULL, SOCK_CLOEXEC)) < 0) {
        return false;
    }
    if (conn >= 0) {
        close(conn);
    }
    conn = c;
    setsockopt(conn, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (taken < 256) {
        taken_at[taken++] = now_ms();
    }
    put("{\"class\":\"VERSION\",\"release\":\"3.25\",\"rev\":\"3.25\",\"proto_major\":3,\"proto_minor\":15}\r\n");
    p = (struct pollfd){conn, POLLIN, 0};
    if (poll(&p, 1, 1000) > 0 && recv(conn, buf, sizeof(buf) - 1, 0) > 0) {
        put("{\"class\":\"DEVICES\",\"devices\":[{\"class\":\"DEVICE\",\"path\":\"%s\",\"driver\":\"NMEA0183\","
            "\"activated\":\"2026-11-13T12:00:00.000Z\"}]}\r\n", PORTFILE);
        put("{\"class\":\"WATCH\",\"enable\":true,\"json\":true,\"nmea\":false,\"raw\":0,\"scaled\":false,"
            "\"timing\":false,\"split24\":false,\"pps\":false}\r\n");
    }
    return true;
}

static int tpv_line(char *buf, size_t size, double lat) {
    return snprintf(buf, size, "{\"class\":\"TPV\",\"device\":\"%s\",\"mode\":3,\"time\":\"2026-11-13T12:00:00.000Z\","
                    "\"lat\":%.7f,\"lon\":4.4000000,\"alt\":10.000}\r\n", PORTFILE, lat);
}

static bool tpv(double lat) {
    char buf[256];

    tpv_line(buf, sizeof(buf), lat);
    return put_raw(buf, strlen(buf));
}

static bool sky(void) {
    return put("{\"class\":\"SKY\",\"device\":\"%s\",\"time\":\"2026-11-13T12:00:00.000Z\",\"hdop\":0.90,\"nSat\":1,\"uSat\":1,"
               "\"satellites\":[{\"PRN\":5,\"el\":45.0,\"az\":90.0,\"ss\":40.0,\"used\":true}]}\r\n", PORTFILE);
}

/* ms until the GUI has this latitude, -1: not within ms */
static int64_t wait_fix(double want, int ms) {
    int64_t t0 = now_ms();

    do {
        double lat, lon;
        int    age;

        if (gps_last_fix(&lat, &lon, &age) && fabs(lat - want) < 1e-8) {
            return now_ms() - t0;
        }
        sleep_ms(5);
    } while (now_ms() - t0 < ms);
    return -1;
}

static int restarts(void) {
    FILE *f = fopen(RESTARTS, "r");
    int   n = 0, c;

    if (!f) {
        return 0;
    }
    while ((c = fgetc(f)) != EOF) {
        n += c == '\n';
    }
    fclose(f);
    return n;
}

/* For ms: every connection of the GUI taken and greeted, nothing else sent; each start of gpsd's time noted */
static int64_t restart_at[16];
static int     restarted;

static void serve(int ms) {
    int64_t end = now_ms() + ms;
    int     seen = restarts();

    restarted = 0;
    while (now_ms() < end) {
        take(10);
        if (restarts() > seen) {
            seen = restarts();
            if (restarted < 16) {
                restart_at[restarted++] = now_ms();
            }
        }
    }
}

static bool near(int64_t got, int64_t want, int64_t slack) {
    return got >= want - slack && got <= want + slack;
}

int main(void) {
    char    *flood;
    size_t   n;
    int64_t  t0, dt;
    double   lat, lon;
    int      age, first, said, lines, before;
    bool     ok;

    if (system("rm -rf test.d && mkdir -p test.d/dev") != 0) {
        return 2;
    }
    log_file = fopen("gps_test.log", "w");
    listen_on();
    gps_init();

    /* The start */
    check(take(2000), "the GUI connects to gpsd at the start");
    check(gps_status() != GPS_STATUS_WORKING, "not working before a report");
    tpv(50.1);
    check(wait_fix(50.1, 1000) >= 0, "the first TPV's position");
    check(gps_status() == GPS_STATUS_WORKING, "working");
    check(gps_last_fix(&lat, &lon, &age) && age == 0 && fabs(lon - 4.4) < 1e-9, "the fix fresh, its longitude");

    /* The GPS window: a TPV and its SKY in one copy, with both's flags */
    window.run = true;
    before = copies(NULL, NULL);
    put("{\"class\":\"TPV\",\"device\":\"%s\",\"mode\":3,\"time\":\"2026-11-13T12:00:01.000Z\",\"lat\":50.2,"
        "\"lon\":4.4}\r\n{\"class\":\"SKY\",\"device\":\"%s\",\"hdop\":0.9,\"nSat\":1,\"uSat\":1,\"satellites\":[{\"PRN\":5,\"el\":45.0,"
        "\"az\":90.0,\"ss\":40.0,\"used\":true}]}\r\n", PORTFILE, PORTFILE);
    sleep_ms(150);
    {
        gps_mask_t set = 0;

        n = copies(&lat, &set) - before;
        check(n == 1 && fabs(lat - 50.2) < 1e-8 && (set & SATELLITE_SET) && (set & TIME_SET) && (set & MODE_SET),
              "the GPS window: a TPV and its SKY in one copy, both's flags (%zu, %.7f, %llx)", n, lat,
              (unsigned long long)set);
    }

    /* Lines faster than ten a second */
    before = copies(NULL, NULL);
    flood = malloc(FLOOD * 200);
    n = 0;
    for (int i = 0; i < FLOOD; i++) {
        n += tpv_line(flood + n, 200, 51.0 + i * 1e-5);
    }
    t0 = now_ms();
    check(put_raw(flood, n), "%d TPVs (%zu bytes) at once taken by the GUI's socket", FLOOD, n);
    dt = wait_fix(51.0 + (FLOOD - 1) * 1e-5, 3000);
    dt = dt < 0 ? -1 : now_ms() - t0;
    check(dt >= 0 && dt < 2000, "all of them read in %lld ms (2 s at most; one every 100 ms: 500 s)", (long long)dt);
    sleep_ms(150);
    n = copies(&lat, NULL) - before;
    check(n >= 1 && n <= 3 && fabs(lat - (51.0 + (FLOOD - 1) * 1e-5)) < 1e-8,
          "to the GPS window in %zu copies, at most one every 100 ms, the last one with the last position", n);
    window.run = false;
    free(flood);
    check(!take(0) && taken == 1, "on the one connection");

    /* gpsd closes the connection */
    close(conn);
    conn = -1;
    t0 = now_ms();
    sleep_ms(300);
    check(gps_status() != GPS_STATUS_WORKING, "not working once gpsd closed the connection");
    ok = take(3000);
    dt = now_ms() - t0;
    check(ok && near(dt, 1000, 300), "gpsd closed the connection: the GUI connects again after 1 s (%lld ms)",
          (long long)dt);
    check(log_count("gpsd closed the connection") == 1, "said in the log");
    tpv(52.0);
    check(wait_fix(52.0, 1000) >= 0 && gps_status() == GPS_STATUS_WORKING, "reports on the new connection");
    sleep_ms(50);
    check(log_count("reports again") == 1, "the log says they came again");

    /* A line longer than libgps's buffer (20 KB): this connection is done */
    flood = malloc(25000);
    memset(flood, 'x', 25000);
    t0 = now_ms();
    put_raw(flood, 25000);
    free(flood);
    ok = take(3000);
    dt = now_ms() - t0;
    check(ok && near(dt, 1000, 300), "a line longer than libgps's buffer: the GUI connects again after 1 s (%lld ms)",
          (long long)dt);
    check(log_count("gpsd closed the connection") == 2, "said in the log");
    tpv(52.5);
    check(wait_fix(52.5, 1000) >= 0, "reports on the new connection");

    /* A GPS that reports, its port there: left alone */
    if (system("touch " PORTFILE) != 0) {
        return 2;
    }
    lines = log_lines();
    first = taken;
    for (int i = 0; i < 20; i++) {
        tpv(53.0 + i * 1e-4);
        if (i % 5 == 0) {
            sky();
        }
        sleep_ms(200);
    }
    check(wait_fix(53.0 + 19 * 1e-4, 1000) >= 0, "a GPS reporting for 4 s, its port there: each report read");
    check(!take(0) && taken == first && restarts() == 0 && log_lines() == lines,
          "the connection kept, gpsd not started again, nothing in the log");

    /* That GPS quiet, its port still there, from its last report on */
    tpv(53.5);
    wait_fix(53.5, 1000);
    t0 = now_ms();
    first = taken;
    said = log_count("connecting to gpsd again");
    serve(6000);
    check(taken - first >= 4 && near(taken_at[first] - t0, 400, 150),
          "the GPS quiet: the GUI connects again after 0.4 s (%lld ms)", (long long)(taken_at[first] - t0));
    check(taken - first >= 4 && near(taken_at[first + 1] - taken_at[first], 400, 150) &&
          near(taken_at[first + 2] - taken_at[first + 1], 800, 150) &&
          near(taken_at[first + 3] - taken_at[first + 2], 1600, 200),
          "then after 0.4, 0.8, 1.6 s (%lld, %lld, %lld ms)",
          (long long)(taken_at[first + 1] - taken_at[first]), (long long)(taken_at[first + 2] - taken_at[first + 1]),
          (long long)(taken_at[first + 3] - taken_at[first + 2]));
    check(log_count("connecting to gpsd again") == said + 1, "said once in the log");
    check(restarted == 2 && near(restart_at[0] - t0, 1600, 250) && near(restart_at[1] - restart_at[0], 3200, 300),
          "gpsd started again after 1.6 s and 3.2 s later (%d: %lld, %lld ms)", restarted,
          (long long)(restarted > 0 ? restart_at[0] - t0 : -1),
          (long long)(restarted > 1 ? restart_at[1] - restart_at[0] : -1));
    check(log_count("gpsd started again") == 2 && log_count("with a GPS on " PORTFILE) == 2,
          "the log says so each time, and the port");

    /* Reports again */
    lines = log_count("reports again");
    tpv(54.0);
    check(wait_fix(54.0, 1000) >= 0 && gps_status() == GPS_STATUS_WORKING, "the GPS reports again: read");
    sleep_ms(50);
    check(log_count("reports again") == lines + 1, "the log says so");

    /* Quiet with the port gone */
    unlink(PORTFILE);
    first = taken;
    before = restarts();
    said = log_count("connecting to gpsd again");
    serve(5000);
    check(restarts() == before, "the GPS unplugged, quiet for 5 s: gpsd not started again");
    check(taken - first >= 4 && taken - first <= 6, "%d connections in 5 s, less and less often", taken - first);
    check(log_count("connecting to gpsd again") == said + 1, "said once in the log");

    /* gpsd gone */
    listen_off();
    close(conn);
    conn = -1;
    first = tries_now();
    t0 = now_ms();
    sleep_ms(5500);
    pthread_mutex_lock(&try_lock);
    check(tries - first == 3 && !try_ok[first] && !try_ok[first + 1] && !try_ok[first + 2] &&
          near(try_at[first] - t0, 1000, 300) && near(try_at[first + 1] - try_at[first], 2000, 300) &&
          near(try_at[first + 2] - try_at[first + 1], 2000, 300),
          "gpsd gone: tried after 1 s, then every 2 s (%d tries)", tries - first);
    pthread_mutex_unlock(&try_lock);
    check(log_count("gpsd not there") == 1, "said once in the log");

    /* A USB device plugged in cuts the pause short */
    first = tries_now();
    while (tries_now() == first && now_ms() - t0 < 9000) {
        sleep_ms(5);
    }
    sleep_ms(200);
    listen_on();
    t0 = now_ms();
    usb_added();
    ok = take(1500);
    dt = now_ms() - t0;
    check(ok && dt < 300, "a USB device plugged in: the GUI tries at once (%lld ms, 1.8 s left of its pause)",
          (long long)dt);
    tpv(55.0);
    check(wait_fix(55.0, 1000) >= 0 && gps_status() == GPS_STATUS_WORKING, "and reports");
    check(log_count("gpsd there again") == 1, "the log says gpsd is there again");

    printf("%d checks, %d failed\n", checks, fails);
    if (fails == 0 && system("rm -rf test.d") != 0) {
        return 2;
    }
    return fails != 0;
}
