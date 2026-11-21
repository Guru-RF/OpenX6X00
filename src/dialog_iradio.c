/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Internet radio (APP > Internet radio): the stations of radio-browser.info, the open directory of
 *  Internet radio stations (your favourites, a country's, the most listened, a search), played on the speaker at
 *  the radio's VOL, or on the Bluetooth headphones while they stand in for it. rootfs/usr/sbin/x6100-iradio fetches
 *  the lists and decodes the station to PCM (MP3 with mpg123, the other formats and HLS with ffmpeg), which this
 *  window's thread sends on as the Recorder plays a recording. On a card image without ffmpeg only MP3 stations are
 *  listed, a favourite of another codec is marked and not played, and the window says a newer image plays them.
 *
 *  Nothing goes out on the network but what is asked for here while the window is open: a list when it is shown
 *  (the helper keeps them an hour), the station while it plays. The station stops when the window closes, at Stop,
 *  and when the radio transmits, whatever keys it: radio.c cuts its audio before its PTT and modem key
 *  (audio_listen_cut), and the thread stops on any TX state, the hand mic's and the KEY jack's too. Nothing here
 *  keys the radio.
 *
 *  From the CX8ANM edition of OpenX6X00 (condoruy, Cóndor Tecnológico): its Internet radio app.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "dialog_iradio.h"
#include "voice.h"
#include "voice_focus.h"

#include "app_frame.h"
#include "audio.h"
#include "board.h"
#include "button_bar.h"
#include "fonts/font_select.h"
#include "gui_events.h"
#include "i18n.h"
#include "iradio_list.h"
#include "key_input.h"
#include "msg_box.h"
#include "radio.h"
#include "scheduler.h"
#include "styles.h"
#include "text_popup.h"
#include "theme.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef X6100_SBIN
#define X6100_SBIN "/usr/sbin"
#endif

#define HELPER      X6100_SBIN "/x6100-iradio"
#define FAV_FILE    BOARD_DATA "/iradio/favorites.tsv"
#define CC_FILE     BOARD_DATA "/iradio/country"
#define STATUS_FILE "/tmp/x6100-iradio.status"      /* the helper's state (rootfs/usr/sbin/x6100-iradio) */
#define FFMPEG      "/usr/bin/ffmpeg"               /* the helper's decoder of every format but MP3, when there */
#define MAX_ST      150         /* stations a list: the helper gives 100 at most, the favourites up to this */
#define MAX_CC      300         /* countries */
#define OUT_MAX     (128 * 1024)    /* the helper's answer, at most */
#define STALL_MS    3000        /* the speaker taking nothing for this long: stopped, with a word */
#define POLL_MS     200         /* the playing thread looks at the TX state at least this often */
#define INFO_W      230         /* the list's second column */
#define NOW_H       64          /* the two lines of the station playing, over the list */
#define TABLE_H     (APP_FRAME_BODY_H - NOW_H - 2)

#define VF(n) (theme_shape.vision ? font_down(&sony_##n, VISION_FONT_STEPS - 1) : &sony_##n)

extern char **environ;

typedef enum { L_FAV, L_COUNTRY, L_TOP, L_SEARCH } list_kind_t;
typedef enum { F_LIST, F_COUNTRIES } fetch_what_t;
typedef enum { P_IDLE, P_CONNECTING, P_PLAYING, P_ERROR } play_state_t;
typedef enum { END_USER, END_TX, END_HELPER, END_STALL, END_WRITE, END_SPAWN } play_end_t;

/* GUI thread, all of these */
static iradio_station_t list[MAX_ST];       /* the stations shown */
static int              list_n, sel;
static list_kind_t      kind;
static char             list_note[96];      /* the table's one row when it has no station: loading, an error */
static iradio_station_t favs[MAX_ST];
static int              favs_n;
static char             country[3], country_name[64];
static char             query[48];
static bool             every;              /* ffmpeg on the image: every format plays, not MP3 only */

static iradio_country_t countries[MAX_CC];
static int              countries_n, cc_sel;
static bool             picking;            /* the table shows the countries */
static bool             kbd_open;           /* the search's text popup */

static iradio_station_t playing;            /* the station playing, or played last */
static play_state_t     pstate;
static char             pdetail[128], ptitle[128];
static iradio_station_t pending;            /* chosen while the last one still stops */
static bool             pending_on;
static int              play_gen;

/* A list being fetched: an answer to an older ask (its gen) is dropped. fetch_pid: the helper running for it */
static atomic_int fetch_gen;
static bool       fetching;
static atomic_int fetch_pid;

typedef struct {
    int          gen;
    fetch_what_t what;
    char         args[2][64];
} fetch_job_t;

typedef struct {
    int          gen;
    fetch_what_t what;
    char        *out;           /* the helper's lines, malloc'd; the receiver frees them */
} fetch_done_t;

/* Playing: one thread at a time. play_on asks it to go on; play_alive is true until it has ended, the speaker given
 * back; tx_seen: the radio transmitted since it started */
static atomic_bool play_on, play_alive, tx_seen;

typedef struct {
    int  gen;
    char url[256];
    char uuid[40];
} play_job_t;

typedef struct {
    int        gen;
    play_end_t why;
} play_done_t;

static app_frame_t frame;
static lv_obj_t   *body, *now_lbl, *info_lbl, *table;
static lv_timer_t *status_timer;

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void play_cb(button_data_t *b);
static void fav_cb(button_data_t *b);
static void list_cb(button_data_t *b);
static void country_cb(button_data_t *b);
static void search_cb(button_data_t *b);
static const char *play_label(void);

static button_data_t btn_play    = {.type = BTN_TEXT_FN, .label_fn = play_label, .press = play_cb};
static button_data_t btn_fav     = {.type = BTN_TEXT, .label = "Favourite", .press = fav_cb};
static button_data_t btn_list    = {.type = BTN_TEXT, .label = "List", .press = list_cb};
static button_data_t btn_country = {.type = BTN_TEXT, .label = "Country", .press = country_cb};
static button_data_t btn_search  = {.type = BTN_TEXT, .label = "Search", .press = search_cb};

static buttons_page_t btn_page = {
    .items = {&btn_play, &btn_fav, &btn_list, &btn_country, &btn_search},
};

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .key_cb       = key_cb,
    .btn_page     = &btn_page,
};

dialog_t *dialog_iradio = &dialog;

/* ---------- the helper ---------- */

/* The helper's state file and its ffmpeg: the same overrides as the helper's, for a run on a PC */
static const char *status_path(void) {
    const char *p = getenv("X6100_IRADIO_STATUS");

    return p && p[0] ? p : STATUS_FILE;
}

static bool ffmpeg_here(void) {
    const char *p = getenv("X6100_IRADIO_FFMPEG");

    return access(p && p[0] ? p : FFMPEG, X_OK) == 0;
}

/* The helper with argv, its stdout on *out: in a process group of its own, so its end takes mpg123 with it; the
 * GUI's blocked and ignored signals back to their defaults (freedv_report.c). -1 when it did not start */
static pid_t spawn_helper(char *const argv[], int *out) {
    int                        p[2];
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t          attr;
    sigset_t                   none, dfl;
    pid_t                      pid;

    if (pipe2(p, O_CLOEXEC) != 0) {
        return -1;
    }
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_init(&attr);
    sigemptyset(&none);
    sigemptyset(&dfl);
    sigaddset(&dfl, SIGTERM);
    sigaddset(&dfl, SIGPIPE);
    sigaddset(&dfl, SIGINT);
    sigaddset(&dfl, SIGCHLD);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &dfl);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);

    int r = posix_spawn(&pid, HELPER, &fa, &attr, argv, environ);

    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(p[1]);
    if (r != 0) {
        close(p[0]);
        return -1;
    }
    *out = p[0];
    return pid;
}

/* The helper and its decoder ended: asked to, then made to after two seconds */
static void end_child(pid_t pid) {
    kill(-pid, SIGTERM);
    for (int i = 0; i < 100; i++) {
        pid_t r = waitpid(pid, NULL, WNOHANG);

        if (r == pid || (r < 0 && errno == ECHILD)) {
            return;
        }
        usleep(20000);
    }
    kill(-pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

/* ---------- lists ---------- */

static void fetch_done_cb(void *arg);

static void *fetch_thread(void *arg) {
    fetch_job_t *job  = arg;
    fetch_done_t done = {.gen = job->gen, .what = job->what};
    char        *argv[] = {(char *)HELPER, job->args[0], job->args[1][0] ? job->args[1] : NULL, NULL};
    int          fd;
    pid_t        pid = -1;

    /* A newer ask before this one started: nothing to fetch */
    if (job->gen == atomic_load(&fetch_gen)) {
        pid = spawn_helper(argv, &fd);
    }
    if (pid > 0) {
        size_t  size = 16 * 1024, used = 0;
        char   *out  = malloc(size);
        ssize_t n    = 0;

        atomic_store(&fetch_pid, pid);
        while (out) {
            if (used + 1 >= size) {
                char *more = size < OUT_MAX ? realloc(out, size * 2) : NULL;

                if (!more) {
                    break;      /* the rest is not read: the helper ends below */
                }
                out = more;
                size *= 2;
            }
            n = read(fd, out + used, size - used - 1);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                break;
            }
            used += n;
        }
        if (out) {
            out[used] = 0;
        }
        close(fd);

        int expected = pid;     /* taken back unless the window's close took it to end it */

        atomic_compare_exchange_strong(&fetch_pid, &expected, 0);
        end_child(pid);
        done.out = out;
    }
    free(job);
    while (!scheduler_put(fetch_done_cb, &done, sizeof(done))) {
        usleep(20000);
    }
    return NULL;
}

/* The list fetched now ended, its answer dropped */
static void stop_fetch(void) {
    pid_t p = atomic_exchange(&fetch_pid, 0);

    atomic_fetch_add(&fetch_gen, 1);
    if (p > 0) {
        kill(-p, SIGTERM);
    }
    fetching = false;
}

static void show(void);

/* The helper's command (and its one argument) on a thread of its own */
static void fetch(fetch_what_t what, const char *cmd, const char *arg) {
    fetch_job_t   *job = calloc(1, sizeof(*job));
    pthread_t      t;
    pthread_attr_t a;

    stop_fetch();
    if (!job) {
        return;
    }
    job->gen  = atomic_load(&fetch_gen);
    job->what = what;
    snprintf(job->args[0], sizeof(job->args[0]), "%s", cmd);
    snprintf(job->args[1], sizeof(job->args[1]), "%s", arg ? arg : "");
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, fetch_thread, job) != 0) {
        free(job);
        snprintf(list_note, sizeof(list_note), "%s", _("The list could not be fetched"));
    } else {
        fetching = true;
        snprintf(list_note, sizeof(list_note), "%s", _("Loading the list from radio-browser.info..."));
    }
    pthread_attr_destroy(&a);
}

static bool is_fav(const char *url) {
    return iradio_find(favs, favs_n, url) >= 0;
}

static void load_favs(void) {
    favs_n = iradio_load(FAV_FILE, favs, MAX_ST);
}

/* The countries' row of the country chosen, the first when it is not in the list */
static int country_row(void) {
    for (int i = 0; i < countries_n; i++) {
        if (!strcmp(countries[i].cc, country)) {
            return i;
        }
    }
    return 0;
}

/* The helper's lines into the list or the countries; its "error <reason>" into the note */
static void fetch_done_cb(void *arg) {
    fetch_done_t *d = arg;

    if (!dialog.run || d->gen != atomic_load(&fetch_gen)) {
        free(d->out);
        return;
    }
    fetching = false;
    if (!d->out) {
        snprintf(list_note, sizeof(list_note), "%s", _("The list could not be fetched"));
    } else if (!strncmp(d->out, "error ", 6)) {
        d->out[strcspn(d->out, "\n")] = 0;
        snprintf(list_note, sizeof(list_note), "%s", _(d->out + 6));
    } else {
        char *save = NULL;
        int   n    = 0;

        for (char *line = strtok_r(d->out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            if (d->what == F_LIST && n < MAX_ST && iradio_parse_station(line, &list[n])) {
                n++;
            } else if (d->what == F_COUNTRIES && n < MAX_CC && iradio_parse_country(line, &countries[n])) {
                n++;
            }
        }
        if (d->what == F_LIST) {
            list_n = n;
            sel    = 0;
        } else {
            countries_n = n;
            cc_sel      = country_row();
        }
        snprintf(list_note, sizeof(list_note), "%s", n ? "" : _("The directory sent no list"));
    }
    free(d->out);
    show();
}

/* ---------- playing ---------- */

/* The station goes on: not stopped, and the radio receives (a PTT, the modem, a TX of the base's own: the hand mic,
 * VOX, the KEY jack, the tuner) */
static bool keep_playing(void) {
    return atomic_load(&play_on) && !atomic_load(&tx_seen) && radio_get_state() == RADIO_RX && !radio_get_modem() &&
           !radio_get_ptt();
}

static void play_done_cb(void *arg);

/* On a stream of its own: the headphones when they stand in for the speaker, else the speaker in Play mode (the base
 * plays Linux's audio on it at the VOL, and gives the receiver's back after). A message of a voice link (Zello) plays
 * over it: the station is dropped meanwhile and goes on after (audio.h) */
static void *play_thread(void *arg) {
    play_job_t  job = *(play_job_t *)arg;
    play_done_t done = {.gen = job.gen, .why = END_HELPER};
    char       *argv[] = {(char *)HELPER, (char *)"play", job.url, job.uuid[0] ? job.uuid : NULL, NULL};
    int         fd;
    bool        started = false;

    free(arg);
    audio_listen_begin(AUDIO_USER_STATION);

    pid_t pid = spawn_helper(argv, &fd);

    if (pid <= 0) {
        done.why = END_SPAWN;
    } else {
        union {
            int16_t s[2048];
            uint8_t b[4096];
        } buf;
        size_t have = 0;    /* bytes: an odd one waits for its pair */

        while (true) {
            if (!keep_playing()) {
                done.why = atomic_load(&play_on) ? END_TX : END_USER;
                break;
            }
            struct pollfd p = {.fd = fd, .events = POLLIN};
            int           r = poll(&p, 1, POLL_MS);

            if (r <= 0) {
                continue;
            }
            ssize_t n = read(fd, buf.b + have, sizeof(buf.b) - have);

            if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
                continue;
            }
            if (n <= 0) {
                break;      /* the helper ended: its state file says why */
            }
            have += n;
            if (have < 2) {
                continue;
            }
            if (!started) {
                audio_listen_open(AUDIO_USER_STATION);    /* its stream and the speaker (or the headphones) */
                started = true;
            }

            int res = audio_listen_play(AUDIO_USER_STATION, buf.s, have / 2, keep_playing, STALL_MS);

            if (res == -1) {    /* stopped, or a key cut it (audio_listen_cut) */
                done.why = atomic_load(&play_on) ? END_TX : END_USER;
                break;
            }
            if (res == -2 || res == -3) {
                done.why = res == -2 ? END_STALL : END_WRITE;
                break;
            }
            if (have & 1) {
                buf.b[0] = buf.b[have - 1];
            }
            have &= 1;
        }
        close(fd);
    }
    audio_listen_end(AUDIO_USER_STATION);     /* the speaker given back, unless a voice link still has it */
    if (pid > 0) {
        end_child(pid);
    }
    atomic_store(&play_on, false);
    atomic_store(&play_alive, false);
    while (!scheduler_put(play_done_cb, &done, sizeof(done))) {
        usleep(20000);
    }
    return NULL;
}

/* The helper's state file: its three lines (state, format, title) into the window's */
static void read_status(void) {
    FILE *f = fopen(status_path(), "r");
    char  line[3][200] = {"", "", ""};

    if (!f) {
        return;
    }
    for (int i = 0; i < 3 && fgets(line[i], sizeof(line[i]), f); i++) {
        line[i][strcspn(line[i], "\r\n")] = 0;
    }
    fclose(f);
    if (!strcmp(line[0], "playing")) {
        pstate = P_PLAYING;
        snprintf(pdetail, sizeof(pdetail), "%s", line[1]);
        snprintf(ptitle, sizeof(ptitle), "%s", line[2]);
    } else if (!strcmp(line[0], "connecting")) {
        pstate = P_CONNECTING;
        snprintf(pdetail, sizeof(pdetail), "%s", line[1]);
        ptitle[0] = 0;
    } else if (!strncmp(line[0], "error ", 6)) {
        pstate = P_ERROR;
        snprintf(pdetail, sizeof(pdetail), "%s", _(line[0] + 6));
        ptitle[0] = 0;
    }
}

static void start_play(const iradio_station_t *s);

static void play_done_cb(void *arg) {
    play_done_t *d = arg;

    if (d->gen != play_gen) {
        return;     /* a later station is on its way */
    }
    if (dialog.run) {
        ptitle[0] = 0;
        switch (d->why) {
            case END_USER:
                pstate = P_IDLE;
                pdetail[0] = 0;
                break;
            case END_TX:
                pstate = P_IDLE;
                snprintf(pdetail, sizeof(pdetail), "%s", _("Stopped: the radio transmits"));
                break;
            case END_HELPER:
                pstate = P_ERROR;
                snprintf(pdetail, sizeof(pdetail), "%s", _("the station stopped sending"));
                read_status();      /* the helper's reason, when it gave one */
                if (pstate != P_ERROR) {
                    pstate = P_ERROR;
                    snprintf(pdetail, sizeof(pdetail), "%s", _("the station stopped sending"));
                }
                break;
            case END_STALL:
                pstate = P_ERROR;
                snprintf(pdetail, sizeof(pdetail), "%s", _("the speaker took no audio"));
                break;
            case END_WRITE:
                pstate = P_ERROR;
                snprintf(pdetail, sizeof(pdetail), "%s", _("the audio could not be played"));
                break;
            case END_SPAWN:
                pstate = P_ERROR;
                snprintf(pdetail, sizeof(pdetail), "%s", _("the player did not start"));
                break;
        }
        if (pending_on) {
            pending_on = false;
            start_play(&pending);
            return;
        }
        buttons_refresh(&btn_play);
        show();
    }
}

static void stop_play(void) {
    pending_on = false;
    atomic_store(&play_on, false);
}

static void start_play(const iradio_station_t *s) {
    play_job_t    *job;
    pthread_t      t;
    pthread_attr_t a;

    if (!iradio_playable(s, every)) {
        msg_update_text_fmt("%s: not on this card image (MP3 only), a newer image plays it", s->codec);
        return;
    }
    if (atomic_load(&play_alive)) {     /* the last one gives the speaker back first */
        pending    = *s;
        pending_on = true;
        atomic_store(&play_on, false);
        return;
    }
    job = calloc(1, sizeof(*job));
    if (!job) {
        return;
    }
    job->gen = ++play_gen;
    atomic_store(&tx_seen, false);
    snprintf(job->url, sizeof(job->url), "%s", s->url);
    snprintf(job->uuid, sizeof(job->uuid), "%s", s->uuid);
    playing = *s;
    pstate  = P_CONNECTING;
    pdetail[0] = ptitle[0] = 0;
    unlink(status_path());      /* the last station's state is not this one's */
    atomic_store(&play_on, true);
    atomic_store(&play_alive, true);
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, play_thread, job) != 0) {
        free(job);
        atomic_store(&play_on, false);
        atomic_store(&play_alive, false);
        pstate = P_ERROR;
        snprintf(pdetail, sizeof(pdetail), "%s", _("the player did not start"));
    }
    pthread_attr_destroy(&a);
    buttons_refresh(&btn_play);
    show();
}

static bool is_playing(const char *url) {
    return atomic_load(&play_on) && !strcmp(playing.url, url);
}

/* ---------- the window ---------- */

static const char *list_title(void) {
    static char title[96];

    if (picking) {
        return _("Countries");
    }
    switch (kind) {
        case L_FAV:
            return _("Favourites");
        case L_COUNTRY:
            return country_name[0] ? country_name : country;
        case L_TOP:
            return _("Most listened");
        case L_SEARCH:
            snprintf(title, sizeof(title), "%s: %s", _("Search"), query);
            return title;
    }
    return "";
}

static int rows_shown(void) {
    return picking ? countries_n : list_n;
}

static int *cur_sel(void) {
    return picking ? &cc_sel : &sel;
}

/* Scrolls only when row r is not wholly in sight */
static void show_row(int r) {
    lv_table_t *t   = (lv_table_t *)table;
    lv_coord_t  y   = 0;
    lv_coord_t  pad = lv_obj_get_style_pad_top(table, LV_PART_MAIN);

    if (r < 0 || r >= (int)t->row_cnt) {
        return;
    }
    for (int i = 0; i < r; i++) {
        y += t->row_h[i];
    }
    lv_coord_t top = pad + y - lv_obj_get_scroll_y(table);

    if (top < 0) {
        lv_obj_scroll_to_y(table, y, LV_ANIM_OFF);
    } else if (top + t->row_h[r] > lv_obj_get_height(table)) {
        lv_obj_scroll_to_y(table, y + t->row_h[r] + 2 * pad - lv_obj_get_height(table), LV_ANIM_OFF);
    }
}

static void table_fill(void) {
    int  n = rows_shown();
    char info[48];

    lv_obj_scroll_to_y(table, 0, LV_ANIM_OFF);
    if (!n) {
        const char *note = list_note[0] ? list_note
                         : kind == L_FAV && !picking
                               ? _("No favourites yet: Favourite keeps the station selected in a list, the web page "
                                   "takes any stream address")
                               : _("No stations");

        lv_table_set_row_cnt(table, 1);
        lv_table_set_cell_value_fmt(table, 0, 0, "%s", note);
        lv_table_set_cell_value(table, 0, 1, "");
        lv_table_clear_cell_ctrl(table, 0, 0, LV_TABLE_CELL_CTRL_TEXT_CROP);
        lv_table_add_cell_ctrl(table, 0, 0, LV_TABLE_CELL_CTRL_MERGE_RIGHT);
        return;
    }
    lv_table_set_row_cnt(table, n);
    lv_table_clear_cell_ctrl(table, 0, 0, LV_TABLE_CELL_CTRL_MERGE_RIGHT);
    for (int r = 0; r < n; r++) {
        if (picking) {
            lv_table_set_cell_value_fmt(table, r, 0, "%s", countries[r].name);
            snprintf(info, sizeof(info), "%s  %u", countries[r].cc, countries[r].stations);
        } else {
            const iradio_station_t *s = &list[r];

            lv_table_set_cell_value_fmt(table, r, 0, "%s%s", kind != L_FAV && is_fav(s->url) ? "* " : "", s->name);
            iradio_info(s, every, info, sizeof(info), _("not playable"));
        }
        lv_table_set_cell_value_fmt(table, r, 1, "%s", info);
        lv_table_add_cell_ctrl(table, r, 0, LV_TABLE_CELL_CTRL_TEXT_CROP);
        lv_table_add_cell_ctrl(table, r, 1, LV_TABLE_CELL_CTRL_TEXT_CROP);
    }
    show_row(*cur_sel());
}

/* A label's text changed only when it differs: a new text starts a scrolling line over */
static void label_set(lv_obj_t *l, const char *text) {
    if (strcmp(lv_label_get_text(l), text)) {
        lv_label_set_text_fmt(l, "%s", text);
    }
}

/* The header, the station playing and its state */
static void show_state(void) {
    char        line[256];
    const char *state;

    switch (pstate) {
        case P_CONNECTING:
            state = _("Connecting");
            break;
        case P_PLAYING:
            state = _("Playing");
            break;
        case P_ERROR:
            state = _("Error");
            break;
        default:
            state = fetching ? _("Loading") : _("Stopped");
            break;
    }
    snprintf(line, sizeof(line), _("Internet radio - %s"), list_title());
    label_set(frame.head_left, line);
    label_set(frame.head_right, state);
    lv_obj_set_style_text_color(frame.head_right, pstate == P_PLAYING ? TC(accent) : pstate == P_ERROR ? TC(err)
                                                                                 : TC(text_head), 0);
    label_set(now_lbl, playing.url[0] ? playing.name : _("Nothing playing"));
    lv_obj_set_style_text_color(info_lbl, pstate == P_ERROR ? TC(err) : TC(text_dim), 0);
    snprintf(line, sizeof(line), "%s%s%s", pdetail, pdetail[0] && ptitle[0] ? "   " : "", ptitle);
    label_set(info_lbl, line);
}

static void show(void) {
    if (!dialog.run || !table) {
        return;
    }
    table_fill();
    show_state();
    lv_obj_invalidate(table);
}

static void table_draw_cb(lv_event_t *e) {
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);

    if (dsc->part != LV_PART_ITEMS || !rows_shown()) {
        return;
    }
    int row = dsc->id / 2, col = dsc->id % 2;

    if (row == *cur_sel()) {
        dsc->rect_dsc->bg_opa       = LV_OPA_COVER;
        dsc->rect_dsc->bg_color     = TC(sel_bg);
        dsc->rect_dsc->border_color = TC(sel_border);
        dsc->rect_dsc->border_width = 1;
        dsc->rect_dsc->border_opa   = LV_OPA_COVER;
        dsc->rect_dsc->border_side  = col ? LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_RIGHT
                                          : LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_LEFT;
        dsc->label_dsc->color       = TC(sel_text);
        return;
    }
    if (picking) {
        if (col) {
            dsc->label_dsc->color = TC(text_dim);
        }
        return;
    }
    if (!iradio_playable(&list[row], every)) {
        dsc->label_dsc->color = TC(text_mute);
    } else if (is_playing(list[row].url)) {
        dsc->label_dsc->color = TC(accent);
    } else if (col) {
        dsc->label_dsc->color = TC(text_dim);
    }
}

static void status_timer_cb(lv_timer_t *t) {
    (void)t;
    if (atomic_load(&play_on)) {
        read_status();
    }
    show_state();
}

/* ---------- choosing ---------- */

/* The country kept on the card for the next start; its line as the countries' (iradio_list.h) */
static void save_country(void) {
    iradio_country_t c = {0};

    snprintf(c.cc, sizeof(c.cc), "%s", country);
    snprintf(c.name, sizeof(c.name), "%s", country_name);
    iradio_save_country(CC_FILE, &c);
}

static void load_country(void) {
    iradio_country_t c;

    country[0] = country_name[0] = 0;
    if (iradio_load_country(CC_FILE, &c)) {
        snprintf(country, sizeof(country), "%s", c.cc);
        snprintf(country_name, sizeof(country_name), "%s", c.name);
    }
}

/* The countries, at the one chosen (the list fetched now sets it when it comes, fetch_done_cb) */
static void open_picker(void) {
    picking = true;
    if (!countries_n) {
        fetch(F_COUNTRIES, "countries", NULL);
    }
    cc_sel = country_row();
    show();
}

static void show_list(list_kind_t k) {
    picking = false;
    kind    = k;
    list_n  = 0;
    sel     = 0;
    list_note[0] = 0;
    stop_fetch();
    switch (k) {
        case L_FAV:
            load_favs();
            memcpy(list, favs, sizeof(list[0]) * favs_n);
            list_n = favs_n;
            for (int i = 0; i < list_n; i++) {
                if (is_playing(list[i].url)) {
                    sel = i;
                }
            }
            break;
        case L_COUNTRY:
            if (!country[0]) {
                open_picker();
                return;
            }
            fetch(F_LIST, "country", country);
            break;
        case L_TOP:
            fetch(F_LIST, "top", NULL);
            break;
        case L_SEARCH:
            fetch(F_LIST, "search", query);
            break;
    }
    show();
}

static void pick_country(int i) {
    if (i < 0 || i >= countries_n) {
        return;
    }
    snprintf(country, sizeof(country), "%s", countries[i].cc);
    snprintf(country_name, sizeof(country_name), "%s", countries[i].name);
    save_country();
    show_list(L_COUNTRY);
}

/* MFK press, Enter, a tap: the country taken; the station played, or stopped when it is the one playing */
static void press(void) {
    if (kbd_open || !rows_shown()) {
        return;
    }
    if (picking) {
        pick_country(cc_sel);
    } else if (is_playing(list[sel].url)) {
        stop_play();
    } else {
        start_play(&list[sel]);
    }
}

static void move_sel(int d) {
    int  n = rows_shown();
    int *s = cur_sel();

    if (!n) {
        return;
    }
    *s = (*s + d + n) % n;
    show_row(*s);
    lv_obj_invalidate(table);

    /* Spoken: the station (or country) and what the list shows of it, "BBC World Service, 64k MP3, 3 of 120" */
    if (*s < lv_table_get_row_cnt(table)) {
        char        text[300];
        const char *c1 = lv_table_get_cell_value(table, *s, 1);

        snprintf(text, sizeof(text), "%s%s%s, %i of %i", lv_table_get_cell_value(table, *s, 0), c1 && c1[0] ? ", " : "",
                 c1 ? c1 : "", *s + 1, n);
        voice_say_clean(text, true);
    }
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    if (kbd_open) {
        return;
    }
    switch (key) {
        case LV_KEY_LEFT:
        case LV_KEY_UP:
            move_sel(-1);
            break;
        case LV_KEY_RIGHT:
        case LV_KEY_DOWN:
            move_sel(1);
            break;
        case LV_KEY_ENTER:
            press();
            break;
        case LV_KEY_ESC:    /* out of the countries to the list before, else the window closed */
            if (picking && (kind != L_COUNTRY || country[0])) {
                show_list(kind);
            } else {
                dialog_destruct();
            }
            break;
        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            break;
        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            break;
        default:
            break;
    }
}

/* The MFK press comes as a click from the encoder; a tap elsewhere on the window does nothing */
static void click_cb(lv_event_t *e) {
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();

    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
        press();
    }
}

static void table_click_cb(lv_event_t *e) {
    uint16_t r, c;

    (void)e;
    lv_table_get_selected_cell(table, &r, &c);
    if (r == LV_TABLE_CELL_NONE || r >= rows_shown()) {
        return;
    }
    *cur_sel() = r;
    press();
    lv_obj_invalidate(table);
}

/* Any TX, whatever keyed it (the hand mic, VOX and the KEY jack too, which radio.c does not see coming): the audio
 * queued dropped at once, the playing thread ends at its next look and says why (play_done_cb) */
static void radio_tx_cb(lv_event_t *e) {
    (void)e;
    pending_on = false;
    if (atomic_load(&play_alive)) {
        atomic_store(&tx_seen, true);
        audio_listen_cut();
    }
}

/* ---------- buttons ---------- */

/* While the search box is open the window's buttons close it, nothing searched, as ESC (Search again too): they acted
 * on the list under the box unseen, or not at all, and the box looked stuck over the window (#316) */
static bool kbd_shut(void) {
    if (!kbd_open) {
        return false;
    }
    textarea_window_cancel();
    return true;
}

static const char *play_label(void) {
    return atomic_load(&play_on) || pending_on ? "Stop" : "Play";
}

static void play_cb(button_data_t *b) {
    if (kbd_shut()) {
        return;
    }
    if (atomic_load(&play_on) || pending_on) {
        stop_play();
    } else {
        press();
    }
    buttons_refresh(b);
}

/* The station selected into the favourites, or out of them; read again first, the web page may have changed them */
static void fav_cb(button_data_t *b) {
    (void)b;
    if (kbd_shut() || picking || !list_n) {
        return;
    }
    iradio_station_t s = list[sel];
    int              i;

    load_favs();
    i = iradio_find(favs, favs_n, s.url);
    if (i >= 0) {
        memmove(&favs[i], &favs[i + 1], sizeof(favs[0]) * (favs_n - i - 1));
        favs_n--;
    } else if (favs_n >= MAX_ST) {
        msg_update_text_fmt("Favourites: %d at most", MAX_ST);
        return;
    } else {
        favs[favs_n++] = s;
    }
    if (!iradio_save(FAV_FILE, favs, favs_n)) {
        msg_update_text_fmt("Favourites could not be saved");
        load_favs();
        return;
    }
    if (i >= 0) {
        msg_update_text_fmt("Out of the favourites: %s", s.name);
    } else {
        msg_update_text_fmt("In the favourites: %s", s.name);
    }
    if (kind == L_FAV) {
        int keep = sel;

        show_list(L_FAV);
        sel = keep < list_n ? keep : (list_n ? list_n - 1 : 0);
    }
    show();
}

/* Favourites, the country's, the most listened, the last search, and round */
static void list_cb(button_data_t *b) {
    (void)b;
    if (kbd_shut()) {
        return;
    }
    list_kind_t next = picking ? kind : (list_kind_t)((kind + 1) % 4);

    if (next == L_SEARCH && !query[0]) {
        next = L_FAV;
    }
    show_list(next);
}

static void country_cb(button_data_t *b) {
    (void)b;
    if (kbd_shut()) {
        return;
    }
    if (picking && country[0]) {
        show_list(L_COUNTRY);
    } else {
        open_picker();
    }
}

static void kbd_end(void) {
    textarea_window_close();
    kbd_open = false;
    lv_group_add_obj(keyboard_group, dialog.obj);
    voice_focus_quiet(dialog.obj);      /* move_sel() speaks the rows */
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);
}

/* A name or a genre to search for; a stream address typed in goes into the favourites, its format told when played */
static bool search_ok(void) {
    char text[256];

    snprintf(text, sizeof(text), "%s", textarea_window_get());
    kbd_end();

    char *t = text + strspn(text, " ");

    t[strcspn(t, "\t\r\n")] = 0;
    for (size_t n = strlen(t); n && t[n - 1] == ' '; n--) {
        t[n - 1] = 0;
    }
    if (!t[0]) {
        return true;
    }
    if (!strncasecmp(t, "http://", 7) || !strncasecmp(t, "https://", 8)) {
        char             line[300];
        iradio_station_t s;
        const char      *host = strstr(t, "//") + 2;

        snprintf(line, sizeof(line), "%.*s\t%s", (int)strcspn(host, "/:?"), host, t);
        if (strchr(t, ' ') || !iradio_parse_station(line, &s)) {
            msg_update_text_fmt("Not a stream address");
            return true;
        }
        load_favs();
        if (iradio_find(favs, favs_n, s.url) < 0 && favs_n < MAX_ST) {
            favs[favs_n++] = s;
            if (!iradio_save(FAV_FILE, favs, favs_n)) {
                msg_update_text_fmt("Favourites could not be saved");
            }
        }
        show_list(L_FAV);
        sel = iradio_find(list, list_n, s.url) >= 0 ? iradio_find(list, list_n, s.url) : 0;
        show();
        return true;
    }
    snprintf(query, sizeof(query), "%s", t);
    show_list(L_SEARCH);
    return true;
}

static bool search_cancel(void) {
    kbd_end();
    return true;
}

/* The box's title is short: the long one ("Station, genre or a stream address (http://...): ") took the whole row and
 * left the text no room, so what was typed, on the on-screen keys or a keyboard, never showed (#316). What to type is
 * the empty box's grey text, in a smaller font to fit */
static void search_cb(button_data_t *b) {
    (void)b;
    if (kbd_shut()) {
        return;
    }
    kbd_open = true;
    lv_group_remove_obj(dialog.obj);
    textarea_window_open_w_label(search_ok, search_cancel, "Search: ");

    lv_obj_t *text = textarea_window_text();

    lv_textarea_set_max_length(text, sizeof(list[0].url) - 1);     /* a stream address, the box's 64 too few */
    lv_textarea_set_placeholder_text(text, "station, genre or http://...");
    lv_obj_set_style_text_font(text, &sony_28, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_color(text, TC(text_mute), LV_PART_TEXTAREA_PLACEHOLDER);
    if (query[0]) {
        textarea_window_set(query);
    }
}

/* ---------- open and close ---------- */

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);
    app_frame_create(&frame, dialog.obj);
    /* Names come from the directory: none of them taken for a #colour */
    lv_label_set_recolor(frame.head_left, false);
    lv_label_set_recolor(frame.head_right, false);
    lv_label_set_long_mode(frame.head_left, LV_LABEL_LONG_DOT);
    lv_label_set_recolor(frame.status, false);
    every = ffmpeg_here();
    if (every) {
        lv_label_set_text(frame.status, "Stations from the directory of radio-browser.info");
    } else {
        lv_label_set_text(frame.status, "MP3 stations only: a newer card image plays the others - radio-browser.info");
    }

    body = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(body);
    app_frame_body(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    now_lbl = lv_label_create(body);
    lv_obj_set_style_text_font(now_lbl, VF(24), 0);
    lv_obj_set_style_text_color(now_lbl, TC(text), 0);
    lv_label_set_long_mode(now_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(now_lbl, 12, 4);
    lv_obj_set_width(now_lbl, APP_FRAME_W - 24);

    info_lbl = lv_label_create(body);
    lv_obj_set_style_text_font(info_lbl, VF(20), 0);
    lv_obj_set_style_text_color(info_lbl, TC(text_dim), 0);
    lv_label_set_long_mode(info_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_pos(info_lbl, 12, 34);
    lv_obj_set_width(info_lbl, APP_FRAME_W - 24);

    table = lv_table_create(body);
    lv_obj_remove_style(table, NULL, LV_STATE_ANY | LV_PART_MAIN);
    lv_obj_set_pos(table, 0, NOW_H);
    lv_obj_set_size(table, APP_FRAME_W - 2, TABLE_H);
    lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_side(table, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_width(table, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(table, TC(dialog_border), LV_PART_MAIN);
    lv_obj_set_style_border_opa(table, 96, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(table, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(table, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_text_font(table, VF(22), LV_PART_ITEMS);
    lv_obj_set_style_text_color(table, TC(text), LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(table, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_hor(table, 8, LV_PART_ITEMS);
    lv_obj_set_scroll_dir(table, LV_DIR_VER);
    lv_table_set_col_cnt(table, 2);
    lv_table_set_col_width(table, 0, APP_FRAME_W - 4 - INFO_W);    /* within the border LVGL counts on both sides */
    lv_table_set_col_width(table, 1, INFO_W);
    lv_obj_add_event_cb(table, table_draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    lv_obj_add_event_cb(table, table_click_cb, LV_EVENT_CLICKED, NULL);

    /* The MFK drives the list: the window itself takes the keys */
    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(dialog.obj, click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(dialog.obj, radio_tx_cb, EVENT_RADIO_TX, NULL);
    voice_focus_quiet(dialog.obj);      /* move_sel() speaks the rows */
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);

    kbd_open = false;
    picking  = false;
    pstate   = P_IDLE;     /* a station of the last time still stopping says nothing more (destruct_cb) */
    pdetail[0] = ptitle[0] = 0;
    memset(&playing, 0, sizeof(playing));
    load_country();
    load_favs();
    /* The favourites when there are any, else the country's, else the countries to choose one */
    show_list(favs_n ? L_FAV : L_COUNTRY);
    status_timer = lv_timer_create(status_timer_cb, 500, NULL);
}

static void destruct_cb(void) {
    stop_play();
    stop_fetch();
    if (kbd_open) {
        textarea_window_close();
        kbd_open = false;
    }
    if (status_timer) {
        lv_timer_del(status_timer);
        status_timer = NULL;
    }
    play_gen++;     /* the ending thread's word is for nobody now */
    body = now_lbl = info_lbl = table = NULL;
}
