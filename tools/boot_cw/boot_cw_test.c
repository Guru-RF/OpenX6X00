/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the boot sound's text (src/boot_cw.c, #289) and of the callsign on the boot logo
 *  (bootanim/boot_call.c, #292).
 *
 *  The sound: Off plays nothing; CQ CQ keys the very code bootanim keyed up to v0.3.12, at 20 WPM; the callsign and
 *  the own text in their place, CQ CQ when there is none or nothing in it to key (a CB call is keyed as any). Letters,
 *  figures, strokes, word gaps and a prosign as the keyer codes them. A text up to BOOT_CW_MAX_MS at 20 WPM; a longer
 *  one faster to fit, never past 40 WPM, then cut after a whole character and not on a stroke; nothing ever longer
 *  than bootanim's sound may be (its lead and the codec's buffer with it). The own text as Settings keeps it:
 *  capitals, one space between words, only what can be keyed, at most BOOT_CW_TEXT_MAX.
 *
 *  The logo: nothing drawn without a callsign or with nothing the font has; a callsign centred over the ring (above
 *  its first row), in the logo's gold where the font inks fully, the rest of the logo untouched; the longest
 *  callsign Settings takes fits the screen.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "boot_call.h"
#include "boot_cw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W       800
#define H       480
#define LEAD_MS 1500    /* bootanim.c: the silence before the CW, */
#define MAX_MS  5700    /* the most its sound may last, */
#define BUF_MS  100     /* the codec's buffer after the last sample */

static int fails;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            fails++;                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

/* text planned: its code, its speed and how much of it went */
static void plan(const char *text, const char *code, int dit_ms, const char *keyed) {
    boot_cw_t cw;

    CHECK(boot_cw_plan(text, &cw), "\"%s\": nothing to key", text);
    if (code) CHECK(!strcmp(cw.code, code), "\"%s\": \"%s\", not \"%s\"", text, cw.code, code);
    CHECK(cw.dit_ms == dit_ms, "\"%s\": a dit of %d ms, not %d", text, cw.dit_ms, dit_ms);
    CHECK(cw.used == strlen(keyed) && !strncmp(text, keyed, cw.used), "\"%s\": \"%.*s\" keyed, not \"%s\"", text,
          (int)cw.used, text, keyed);
    CHECK(cw.dits * cw.dit_ms <= BOOT_CW_MAX_MS, "\"%s\": %d ms", text, cw.dits * cw.dit_ms);
    CHECK(LEAD_MS + cw.dits * cw.dit_ms + BUF_MS < MAX_MS, "\"%s\": the sound past bootanim's limit", text);
}

static void clean(const char *in, const char *want) {
    char out[BOOT_CW_TEXT_MAX + 1];

    boot_cw_clean(in, out);
    CHECK(!strcmp(out, want), "clean \"%s\": \"%s\", not \"%s\"", in, out, want);
}

static void sound(void) {
    boot_cw_t cw;
    char      all[512] = "";

    /* What plays */
    CHECK(boot_cw_text(BOOT_SOUND_OFF, "ON6URE", "VVV") == NULL, "Off plays");
    CHECK(!strcmp(boot_cw_text(BOOT_SOUND_CQ, "ON6URE", "VVV"), "CQ CQ"), "CQ CQ");
    CHECK(!strcmp(boot_cw_text(BOOT_SOUND_CALL, "ON6URE", "VVV"), "ON6URE"), "the callsign");
    CHECK(!strcmp(boot_cw_text(BOOT_SOUND_CALL, "", "VVV"), "CQ CQ"), "no callsign: CQ CQ");
    CHECK(!strcmp(boot_cw_text(BOOT_SOUND_CALL, NULL, "VVV"), "CQ CQ"), "no callsign read: CQ CQ");
    CHECK(!strcmp(boot_cw_text(BOOT_SOUND_CALL, "26AT101", ""), "26AT101"), "a CB callsign");
    CHECK(!strcmp(boot_cw_text(BOOT_SOUND_TEXT, "ON6URE", "VVV DE ON6URE"), "VVV DE ON6URE"), "the own text");
    CHECK(!strcmp(boot_cw_text(BOOT_SOUND_TEXT, "ON6URE", ""), "CQ CQ"), "no own text: CQ CQ");
    CHECK(!strcmp(boot_cw_text(BOOT_SOUND_TEXT, "ON6URE", " <> "), "CQ CQ"), "nothing to key: CQ CQ");
    CHECK(!strcmp(boot_cw_text(7, "ON6URE", "VVV"), "CQ CQ"), "a choice of a later version: CQ CQ");
    CHECK(!boot_cw_plan("", &cw) && !boot_cw_plan("  <> ", &cw), "nothing planned from nothing");

    /* CQ CQ as bootanim keyed it up to v0.3.12, 3.72 s */
    plan("CQ CQ", "-.-. --.-   -.-. --.-", 60, "CQ CQ");
    boot_cw_plan("CQ CQ", &cw);
    CHECK(cw.dits == 62, "CQ CQ is %d dits", cw.dits);

    /* Callsigns and texts at 20 WPM, small letters, word gaps, what has no code left out */
    plan("ON6URE", "--- -. -.... ..- .-. .", 60, "ON6URE");
    plan("on6ure", "--- -. -.... ..- .-. .", 60, "on6ure");
    plan("K #", "-.-", 60, "K");
    plan("#K", "-.-", 60, "#K");

    /* Longer: faster to fit (3900 ms / its dits), the code the same; a CB call's figures are long, a prosign */
    plan("CX8ANM", "-.-. -..- ---.. .- -. --", 55, "CX8ANM");                    /* 70 dits */
    plan("DE  ON6URE", "-.. .   --- -. -.... ..- .-. .", 51, "DE  ON6URE");                /* 76, one word gap */
    plan("26AT101", "..--- -.... .- - .---- ----- .----", 36, "26AT101");       /* 106 */
    plan("DE ON6URE <SK>", "-.. .   --- -. -.... ..- .-. .   ...-.-", 39, "DE ON6URE <SK>");
    plan("VVV DE ON6URE", NULL, 33, "VVV DE ON6URE");
    /* At 40 WPM and still too long: cut, not on a stroke */
    plan("EA8/ON6URE/P", ". .- ---.. -..-. --- -. -.... ..- .-. .", 30, "EA8/ON6URE");
    plan("0000000000000000", NULL, 30, "000000");
    /* Every character the own text may hold, alone and in the longest text: none longer than the limit */
    for (const char *c = BOOT_CW_TEXT_CHARS; *c; c++) {
        char one[2] = {*c, 0};

        if (*c == ' ' || *c == '<' || *c == '>') continue;
        CHECK(boot_cw_plan(one, &cw) && cw.used == 1, "'%c' has no code", *c);
        for (int i = 0; i < 4; i++) strncat(all, one, sizeof(all) - strlen(all) - 1);
    }
    CHECK(boot_cw_plan(all, &cw) && cw.dit_ms == 30 && cw.dits * 30 <= BOOT_CW_MAX_MS, "everything: %d dits at %d ms",
          cw.dits, cw.dit_ms);

    /* The own text as kept */
    clean("vvv de on6ure", "VVV DE ON6URE");
    clean("  CQ   CQ  ", "CQ CQ");
    clean("QRV? 73!", "QRV? 73");
    clean("\xc3\xa9t\xc3\xa9 <SK>", "T <SK>");
    clean("ABCDEFGHIJKLMNOPQRSTU", "ABCDEFGHIJKLMNOP");
    clean("ABCDEFGHIJKLMNO PQ", "ABCDEFGHIJKLMNO");
    clean("", "");
}

static uint8_t logo[W * H * 3];

static void ground(void) {
    for (int i = 0; i < W * H; i++) {
        logo[3 * i] = 8;    /* the logo's (tools/logo/mklogo.py) */
        logo[3 * i + 1] = 9;
        logo[3 * i + 2] = 12;
    }
}

/* The box of what was drawn: false when nothing */
static int drawn(int *x1, int *y1, int *x2, int *y2, int *full) {
    int n = 0;

    *x1 = W, *y1 = H, *x2 = -1, *y2 = -1, *full = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const uint8_t *p = logo + (y * W + x) * 3;

            if (p[0] == 8 && p[1] == 9 && p[2] == 12) continue;
            n++;
            if (x < *x1) *x1 = x;
            if (x > *x2) *x2 = x;
            if (y < *y1) *y1 = y;
            if (y > *y2) *y2 = y;
            if (p[0] == 229 && p[1] == 168 && p[2] == 35) (*full)++;
        }
    }
    return n;
}

static void call(const char *c, bool want) {
    int x1, y1, x2, y2, full;

    ground();
    CHECK(boot_call_draw(logo, W, H, c) == want, "\"%s\": drawn %d", c ? c : "(null)", !want);
    int n = drawn(&x1, &y1, &x2, &y2, &full);

    if (!want) {
        CHECK(n == 0, "\"%s\": %d pixels", c ? c : "(null)", n);
        return;
    }
    CHECK(y1 >= 4 && y2 < BOOT_CALL_BOTTOM, "\"%s\": rows %d to %d", c, y1, y2);
    CHECK(x1 >= 20 && x2 < W - 20, "\"%s\": columns %d to %d", c, x1, x2);
    CHECK(abs(x1 - (W - 1 - x2)) <= 4, "\"%s\": not centred, %d left, %d right", c, x1, W - 1 - x2);
    CHECK(full > 100, "\"%s\": %d pixels in the gold", c, full);
}

static void logo_call(void) {
    call(NULL, false);
    call("", false);
    call("#@!", false);
    call("ON6URE", true);
    call("on6ure", true);
    call("26AT101", true);
    call("EA8/ON6URE/P", true);
    call("WWWWWWWWWWWW", true);     /* the widest of Settings' 12 */
    CHECK(!boot_call_draw(logo, W, BOOT_CALL_BOTTOM - 1, "ON6URE"), "drawn on a logo too low");
}

/* --draw in.rgb out.rgb CALL: CALL on a logo, to look at */
static int draw(const char *in, const char *out, const char *c) {
    FILE *f = fopen(in, "rb");

    if (!f || fread(logo, 1, sizeof(logo), f) != sizeof(logo)) return 1;
    fclose(f);
    boot_call_draw(logo, W, H, c);
    f = fopen(out, "wb");
    if (!f || fwrite(logo, 1, sizeof(logo), f) != sizeof(logo)) return 1;
    return fclose(f) != 0;
}

int main(int argc, char **argv) {
    if (argc == 5 && !strcmp(argv[1], "--draw")) return draw(argv[2], argv[3], argv[4]);
    sound();
    logo_call();
    printf("%s\n", fails ? "boot_cw: FAILED" : "boot_cw: all passed");
    return fails != 0;
}
