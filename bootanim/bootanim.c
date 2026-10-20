/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00: boot animation and boot sound
 *
 *  The boot logo composes itself on the framebuffer while the radio boots.
 *  The kernel logo is the first frame (the plain background), then every
 *  logo pixel fades in from its own start time: the ring grows out from its
 *  centre, the signal runs through it left to right and out of the gap, the
 *  name sweeps in, the rule draws from the centre, the subtitle last.
 *  It ends on the full logo, which stays until the GUI takes the screen.
 *
 *  With Settings > Audio > Boot sound on (read from the GUI's params.db),
 *  "CQ CQ" in CW plays on the speaker with the animation.
 *
 *  Started in the background by /etc/init.d/S00bootanim. Any failure just
 *  skips that part: the boot never waits for it.
 */

#include <errno.h>
#include <fcntl.h>
#include <gpiod.h>
#include <linux/fb.h>
#include <alsa/asoundlib.h>
#include <aether_radio/x6100_control/control.h>
#include <math.h>
#include <png.h>
#include <pthread.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define W        800    /* the logo, landscape */
#define H        480
#define SLOW(ms) ((ms) * 28 / 19)   /* the 1.9 s of steps played over 2.8 s */
#define FADE_MS  SLOW(180)          /* each pixel fades in over this */
#define FRAME_MS 40     /* 25 frames per second */
#define CX       400    /* the ring's centre (tools/logo/mklogo.py) */
#define CY       143
#define GUI_UP   "/tmp/x6100_gui.drm"   /* written by the GUI once it owns the display */

static volatile sig_atomic_t stop;      /* SIGTERM from S49bootanim: the codec is needed, the sound stops */

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

/* ---- Boot sound ----------------------------------------------------------- */

#define PARAMS_DB   "/mnt/params.db"
#define AUDIO_BUSY  "/tmp/x6100-bootanim.audio" /* while the codec is open: S49bootanim waits */
#define RATE        44100
#define TONE_HZ     600
#define DIT_MS      60        /* 20 WPM */
#define EDGE_MS     5         /* raised-cosine keying edges */
#define AMPL        20675     /* -4 dBFS */
#define LEAD_MS     1500      /* silence first: the base opens its speaker path late (400 ms still cut the first C) */
#define MIN_VOL     28        /* AF for the sound when the saved VOL (0..55) is lower */
#define VOL_DEFAULT 30        /* p_volume's default, when the DB has none */
#define SOUND_MAX_MS 5700     /* CQ CQ is 5.16 s with the lead: the sound never runs longer than this */

/* The base unit, through the GUI's own control library: x6100_control_init()
 * sends every register with the library's defaults (RX, PTT off), as the GUI
 * does first thing at its start; voice_rec alone then makes the base play the
 * codec on the speaker. The GUI repeats x6100_control_init() when it starts */
static int user_vol;

/* The CW key line held up (1) while the base is ours, as the GUI sets it before
 * x6100_control_init() (radio.c). Only this line: the library's x6100_gpio_init()
 * would also drive the backlight, USB and WiFi lines. Released at the end, so the
 * GUI can claim it */
#define KEY_CHIP "gpiochip1"
#define KEY_LINE 203                  /* X6100_PIN_MORSE_KEY */
static struct gpiod_chip *key_chip;
static struct gpiod_line *key_line;

static void key_release(void) {
    if (key_line) gpiod_line_release(key_line);
    if (key_chip) gpiod_chip_close(key_chip);
    key_line = NULL;
    key_chip = NULL;
}

static int key_up(void) {
    struct gpiod_line *l;

    if (!(key_chip = gpiod_chip_open_by_name(KEY_CHIP))) return 0;
    if (!(l = gpiod_chip_get_line(key_chip, KEY_LINE)) || gpiod_line_request_output(l, "x6100-bootanim", 1) != 0) {
        key_release();
        return 0;
    }
    key_line = l;
    return 1;
}
static int16_t cq[RATE * 6];  /* "CQ CQ" is 5.16 s with the lead */
static int     cq_len;

/* A setting as saved by the GUI (def when it has none), -1 when the DB can't be read */
static int saved(const char *name, int def) {
    sqlite3      *db;
    sqlite3_stmt *st;
    int           val = -1;

    if (sqlite3_open_v2(PARAMS_DB, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK &&
        sqlite3_prepare_v2(db, "SELECT val FROM params WHERE name = ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        val = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : def;
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return val;
}

/* "-.-. --.-" keyed: ' ' adds 2 dits to the 1 after each element (3 between letters, 7 for "   ") */
static void cw(const char *code) {
    const int dit = RATE * DIT_MS / 1000, edge = RATE * EDGE_MS / 1000;

    cq_len = RATE * LEAD_MS / 1000;
    for (const char *c = code; *c; c++) {
        if (*c == ' ') {
            cq_len += 2 * dit;
            continue;
        }
        int len = (*c == '-' ? 3 : 1) * dit;
        for (int i = 0; i < len; i++) {
            float env = 1.0f;
            if (i < edge) env = 0.5f - 0.5f * cosf(M_PI * i / edge);
            else if (i >= len - edge) env = 0.5f - 0.5f * cosf(M_PI * (len - i) / edge);
            cq[cq_len + i] = (int16_t)(AMPL * env * sinf(2.0f * M_PI * TONE_HZ * i / RATE));
        }
        cq_len += len + dit;
    }
}

/* The codec as PulseAudio runs it: playback and capture together, stereo. Opened
 * non-blocking: nothing here ever waits on it, and the whole sound has a deadline */
static snd_pcm_t *play_pcm, *rec_pcm;
static int64_t    deadline;

static int pcm_open(snd_pcm_t **p, snd_pcm_stream_t dir) {
    if (snd_pcm_open(p, "plughw:0,0", dir, SND_PCM_NONBLOCK) < 0) {
        *p = NULL;
        return 0;
    }
    return snd_pcm_set_params(*p, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, RATE, 1, 100000) == 0;
}

static void pcm_close(void) {
    if (play_pcm) { snd_pcm_drop(play_pcm); snd_pcm_close(play_pcm); }
    if (rec_pcm)  { snd_pcm_drop(rec_pcm);  snd_pcm_close(rec_pcm); }
    play_pcm = rec_pcm = NULL;
}

/* 100 ms of silence in: the codec runs when it has played 20 ms of it within 300 ms */
static int codec_runs(void) {
    static int16_t z[RATE / 10 * 2];
    snd_pcm_sframes_t d, k;

    snd_pcm_start(rec_pcm);
    if ((k = snd_pcm_writei(play_pcm, z, RATE / 10)) <= 0) return 0;
    for (int64_t until = now_ms() + 300; now_ms() < until; usleep(10000)) {
        snd_pcm_readi(rec_pcm, z, RATE / 100);          /* capture read and thrown away */
        if (snd_pcm_delay(play_pcm, &d) == 0 && d < k - RATE / 50) return 1;
    }
    return 0;
}

static void *sound_thread(void *arg) {
    static int16_t st[441 * 2], sink[441 * 2];
    snd_pcm_sframes_t d;
    int at = 0;

    (void)arg;
    while (at < cq_len && !stop && now_ms() < deadline && access(GUI_UP, F_OK) != 0) {
        int n = cq_len - at < 441 ? cq_len - at : 441;
        for (int i = 0; i < n; i++) st[2 * i] = st[2 * i + 1] = cq[at + i];
        snd_pcm_readi(rec_pcm, sink, 441);
        snd_pcm_sframes_t k = snd_pcm_writei(play_pcm, st, n);
        if (k > 0) at += k;
        else if (k == -EAGAIN) usleep(5000);
        else if (snd_pcm_recover(play_pcm, k, 1) < 0) break;
    }
    while (!stop && now_ms() < deadline && snd_pcm_delay(play_pcm, &d) == 0 && d > 0) {
        snd_pcm_readi(rec_pcm, sink, 441);
        usleep(10000);
    }
    pcm_close();
    return NULL;
}

/* True when the sound started: codec running, base in play mode */
static int sound_start(pthread_t *th) {
    if (saved("boot_sound", 0) != 1) return 0;
    user_vol = saved("vol", VOL_DEFAULT);
    if (user_vol < 0 || user_vol > 55) return 0;
    close(open(AUDIO_BUSY, O_CREAT | O_WRONLY, 0644));
    /* The codec levels as the GUI sets them (audio.c audio_mixer_setup); ALSA restores the rest later */
    system("alsactl restore >/dev/null 2>&1; amixer -q sset 'Headphone',0 63,63 >/dev/null 2>&1; "
           "amixer -q sset 'AIF1 DA0',0 160,160 >/dev/null 2>&1");
    if (!pcm_open(&play_pcm, SND_PCM_STREAM_PLAYBACK) || !pcm_open(&rec_pcm, SND_PCM_STREAM_CAPTURE) ||
        !codec_runs()) {
        goto no_sound;                       /* the base is not touched */
    }
    if (access(GUI_UP, F_OK) == 0) goto no_sound;
    if (!key_up()) goto no_sound;
    if (!x6100_control_init()) goto no_sound;   /* once: no retries */
    x6100_control_rxvol_set(user_vol < MIN_VOL ? MIN_VOL : user_vol);
    x6100_control_record_set(true);
    cw("-.-. --.-   -.-. --.-");
    deadline = now_ms() + SOUND_MAX_MS;
    if (pthread_create(th, NULL, sound_thread, NULL) == 0) return 1;
    x6100_control_record_set(false);
    x6100_control_rxvol_set(user_vol);
no_sound:
    key_release();
    pcm_close();
    unlink(AUDIO_BUSY);
    return 0;
}

/* Codec closed (by the thread), play mode off, the user's AF back; nothing once the GUI is up */
static void sound_end(pthread_t th) {
    pthread_join(th, NULL);
    if (access(GUI_UP, F_OK) != 0) {
        x6100_control_record_set(false);
        x6100_control_rxvol_set(user_vol);
    }
    key_release();
    unlink(AUDIO_BUSY);
}

/* ---- Animation ------------------------------------------------------------ */

/* When a logo pixel starts to fade in, ms from the first frame. The mark is 190 px wide at the
 * top (y 48..238): its ring is gold, the signal through it blue (b > r); then the name (y 258..340),
 * the rule (y 352) and the subtitle (y 372..400). */
static int start_ms(int x, int y, const uint8_t *p) {
    if (y < 250) {
        double r = hypot(x - CX, y - CY);

        if (p[2] > p[0]) return 560 + (x - 300) * 2;                 /* the signal, left to right */
        return 120 + (int)(r * 4);                                   /* the ring, from the centre out */
    }
    if (y < 350) return 820 + (x - 160) * 420 / 480 + (345 - y);     /* the name, a diagonal sweep */
    if (y < 362) return 1300 + abs(x - 400) * 260 / 60;              /* the rule, from the centre */
    return 1500 + (x - 85) * 220 / 630;                              /* the subtitle */
}

static void on_term(int sig) {
    (void)sig;
    stop = 1;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/usr/share/x6100/bootlogo.png";
    png_image   img  = {.version = PNG_IMAGE_VERSION};
    static uint8_t logo[W * H * 3];

    if (access(GUI_UP, F_OK) == 0) return 0;
    signal(SIGTERM, on_term);
    if (!png_image_begin_read_from_file(&img, path)) return 1;
    img.format = PNG_FORMAT_RGB;
    if (img.width != W || img.height != H || !png_image_finish_read(&img, NULL, logo, 0, NULL)) return 1;

    /* The panel is 480x800 portrait: landscape (x, y) is fb (y, 799 - x), as the kernel logo */
    int fd = open("/dev/fb0", O_RDWR);
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f) ||
        v.xres != H || v.yres != W || v.bits_per_pixel != 32) return 1;
    uint8_t *fb = mmap(NULL, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) return 1;
    #define PIX(r, g, b) ((uint32_t)(r) << v.red.offset | (uint32_t)(g) << v.green.offset | (uint32_t)(b) << v.blue.offset)

    /* Only the pixels that differ from the background change */
    const uint8_t *bg = logo;
    static uint32_t off[W * H];
    static int16_t  t0[W * H];
    static uint32_t src[W * H];
    int n = 0, end = 0;

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const uint8_t *p = logo + (y * W + x) * 3;
            uint32_t       o = (W - 1 - x) * f.line_length + y * 4;

            *(uint32_t *)(fb + o) = PIX(bg[0], bg[1], bg[2]);
            if (p[0] == bg[0] && p[1] == bg[1] && p[2] == bg[2]) continue;
            off[n] = o;
            t0[n]  = SLOW(start_ms(x, y, p));
            src[n] = (y * W + x) * 3;
            if (t0[n] + FADE_MS > end) end = t0[n] + FADE_MS;
            n++;
        }
    }

    pthread_t th;
    int       sound = sound_start(&th);
    /* With the sound the animation starts later, so the last element ends on the full logo
     * (cq_len has one dit of silence at the end; 100 ms is the codec buffer) */
    int64_t   start = now_ms() + (sound ? (cq_len - RATE * DIT_MS / 1000) * 1000LL / RATE + 100 - end : 0);
    int64_t   wait0 = start - now_ms();
    if (wait0 > 0) usleep(wait0 * 1000);
    for (int t = 0;; t += FRAME_MS) {
        if (t > end) t = end;
        for (int i = 0; i < n; i++) {
            int a = (t - t0[i]) * 256 / FADE_MS;
            if (a <= 0) continue;
            if (a > 256) a = 256;
            const uint8_t *p = logo + src[i];
            *(uint32_t *)(fb + off[i]) = PIX(bg[0] + ((p[0] - bg[0]) * a >> 8),
                                             bg[1] + ((p[1] - bg[1]) * a >> 8),
                                             bg[2] + ((p[2] - bg[2]) * a >> 8));
        }
        if (t == end || access(GUI_UP, F_OK) == 0) break;
        int64_t wait = start + t + FRAME_MS - now_ms();
        if (wait > 0) usleep(wait * 1000);
    }
    if (sound) {
        if (access(GUI_UP, F_OK) == 0) stop = 1;   /* the GUI is up: it needs the codec and the base */
        sound_end(th);
    }
    return 0;
}
