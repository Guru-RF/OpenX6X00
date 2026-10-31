/* bootanim/bootanim.c on the PC (#101), linked with this file and -Wl,--wrap for what it asks of the radio: the
 * framebuffer is a file (the 480x800 portrait panel, 32 bits a pixel, red at bit 16), its two pictures are the
 * files the environment names, its clock moves only when it waits (so every frame is 40 ms), and params.db is
 * never there (no boot sound, the base and the codec untouched). Run as the program itself:
 *   OWN=<file> OURS=<file> FB=<file> [GUI_UP=1] [SNAP=<k>] bootanim [picture]
 * OWN stands for /mnt/bootlogo.png, OURS for /usr/share/x6100/bootlogo.png; each picture it opens is printed
 * ("read /mnt/bootlogo.png"). SNAP=k copies the framebuffer to <FB>.snap as it is at the k-th wait, after the
 * frame of (k - 1) * 40 ms */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <png.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "bootanim-stubs.h"

#define FB_W 480
#define FB_H 800

int __real_open(const char *path, int flags, ...);
int __real_ioctl(int fd, unsigned long req, ...);
int __real_access(const char *path, int mode);
int __real_png_image_begin_read_from_file(png_imagep image, const char *path);

int __wrap_open(const char *path, int flags, ...) {
    va_list ap;
    int     mode;

    va_start(ap, flags);
    mode = flags & O_CREAT ? va_arg(ap, int) : 0;
    va_end(ap);
    if (strcmp(path, "/dev/fb0") == 0) path = getenv("FB");
    else if (strcmp(path, "/mnt/bootlogo.png") == 0 && getenv("OWN")) path = getenv("OWN");
    else if (strcmp(path, "/usr/share/x6100/bootlogo.png") == 0 && getenv("OURS")) path = getenv("OURS");
    return __real_open(path, flags, mode);
}

int __wrap_ioctl(int fd, unsigned long req, ...) {
    va_list ap;
    void   *arg;

    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);
    if (req == FBIOGET_VSCREENINFO) {
        struct fb_var_screeninfo *v = arg;
        memset(v, 0, sizeof(*v));
        v->xres = v->xres_virtual = FB_W;
        v->yres = v->yres_virtual = FB_H;
        v->bits_per_pixel = 32;
        v->red.offset = 16;
        v->green.offset = 8;
        v->red.length = v->green.length = v->blue.length = 8;
        return 0;
    }
    if (req == FBIOGET_FSCREENINFO) {
        struct fb_fix_screeninfo *f = arg;
        memset(f, 0, sizeof(*f));
        f->line_length = FB_W * 4;
        f->smem_len = FB_W * 4 * FB_H;
        return 0;
    }
    return __real_ioctl(fd, req, arg);
}

int __wrap_access(const char *path, int mode) {
    if (strcmp(path, "/tmp/x6100_gui.drm") == 0) {
        if (getenv("GUI_UP")) return 0;
        errno = ENOENT;
        return -1;
    }
    return __real_access(path, mode);
}

int __wrap_png_image_begin_read_from_file(png_imagep image, const char *path) {
    const char *file = path;

    printf("read %s\n", path);
    fflush(stdout);
    if (strcmp(path, "/mnt/bootlogo.png") == 0) file = getenv("OWN");
    else if (strcmp(path, "/usr/share/x6100/bootlogo.png") == 0) file = getenv("OURS");
    return __real_png_image_begin_read_from_file(image, file ? file : "/nonexistent/picture.png");
}

int __wrap_sqlite3_open_v2(const char *name, sqlite3 **db, int flags, const char *vfs) {
    (void)name;
    (void)flags;
    (void)vfs;
    *db = NULL;
    return SQLITE_CANTOPEN;
}

/* The clock: moved by the waits alone */
static long long fake_ns = 1000LL * 1000000000LL;
static int       waits;

int __real_clock_gettime(clockid_t id, struct timespec *ts);

int __wrap_clock_gettime(clockid_t id, struct timespec *ts) {
    if (id != CLOCK_MONOTONIC) return __real_clock_gettime(id, ts);
    ts->tv_sec = fake_ns / 1000000000LL;
    ts->tv_nsec = fake_ns % 1000000000LL;
    return 0;
}

int __wrap_usleep(useconds_t us) {
    const char *snap = getenv("SNAP");

    fake_ns += us * 1000LL;
    if (snap && ++waits == atoi(snap)) {
        char  name[4096];
        FILE *in = fopen(getenv("FB"), "rb"), *out;
        static char buf[FB_W * 4 * FB_H];

        snprintf(name, sizeof(name), "%s.snap", getenv("FB"));
        out = fopen(name, "wb");
        if (in && out) fwrite(buf, 1, fread(buf, 1, sizeof(buf), in), out);
        if (in) fclose(in);
        if (out) fclose(out);
    }
    return 0;
}

/* Never called while the boot sound stays off: said, if one is */
static void touched(const char *what) {
    printf("touched %s\n", what);
    fflush(stdout);
}

struct gpiod_chip *gpiod_chip_open_by_name(const char *name) { (void)name; touched("the key line"); return NULL; }
struct gpiod_line *gpiod_chip_get_line(struct gpiod_chip *c, unsigned int o) { (void)c; (void)o; return NULL; }
int gpiod_line_request_output(struct gpiod_line *l, const char *c, int v) { (void)l; (void)c; (void)v; return -1; }
void gpiod_line_release(struct gpiod_line *l) { (void)l; }
void gpiod_chip_close(struct gpiod_chip *c) { (void)c; }
bool x6100_control_init(void) { touched("the base"); return false; }
void x6100_control_rxvol_set(uint8_t vol) { (void)vol; touched("the base"); }
void x6100_control_record_set(bool on) { (void)on; touched("the base"); }
bool x6200_control_init(void) { touched("the base"); return false; }
void x6200_control_rxvol_set(uint8_t vol) { (void)vol; touched("the base"); }
void x6200_control_record_set(bool on) { (void)on; touched("the base"); }
