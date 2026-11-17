/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The boot sound's text (Settings > Audio > Boot sound, #289): "CQ CQ" as up to v0.3.12, the callsign, or a text of
 *  the owner's, as the elements bootanim keys and the speed it keys them at. At 20 WPM when it takes no longer than
 *  BOOT_CW_MAX_MS ("CQ CQ" takes 3.72 s); a longer one faster to fit, up to 40 WPM; what still does not fit is left
 *  out, whole characters. So the sound is never longer than bootanim's own limit lets it be, and the boot waits for
 *  nothing (S49bootanim gives it 3.5 s at most).
 *
 *  No LVGL and no ALSA: bootanim and the GUI's Settings share it (host test: tools/boot_cw). The codes are the
 *  keyer's (cw_decoder/morse_code.c).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Settings > Audio > Boot sound (p_boot_sound): 1 was the switch's "on" up to v0.3.12, so a saved one stays CQ CQ */
typedef enum {
    BOOT_SOUND_OFF,
    BOOT_SOUND_CQ,      /* "CQ CQ" */
    BOOT_SOUND_CALL,    /* the callsign of Settings > General > STATION */
    BOOT_SOUND_TEXT,    /* the own text, p_boot_text */
    BOOT_SOUNDS
} boot_sound_t;

#define BOOT_CW_CQ          "CQ CQ"
#define BOOT_CW_TEXT_MAX    16      /* the own text's characters */
/* What the own text may hold: the keyer has a code for each; small letters are taken as capitals. < and > for a
 * prosign, <SK> */
#define BOOT_CW_TEXT_CHARS  "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,?/=-<>"
#define BOOT_CW_DIT_MS      60      /* 20 WPM */
#define BOOT_CW_DIT_MIN_MS  30      /* 40 WPM: the fastest a long text goes */
#define BOOT_CW_MAX_MS      3900    /* the keying, its last gap included; bootanim's sound has 5.7 s in all, the
                                     * 1.5 s lead and the codec's 0.1 s buffer with it */
#define BOOT_CW_CODE_MAX    256

typedef struct {
    char   code[BOOT_CW_CODE_MAX];  /* '.' a dit and '-' a dah, a dit's gap after each; ' ' two dits more: a letter's
                                     * gap of 3 dits, "   " a word's of 7. "CQ CQ" is "-.-. --.-   -.-. --.-" */
    int    dit_ms;                  /* BOOT_CW_DIT_MS, or shorter to fit */
    int    dits;                    /* how long it all is, in dits */
    size_t used;                    /* the bytes of the text keyed: less than its length when the rest did not fit */
} boot_cw_t;

/* The text the boot sound keys for choice (boot_sound_t): the callsign or the own text when it has something to key,
 * else "CQ CQ" (no callsign set, an empty text); NULL for Off */
const char *boot_cw_text(int choice, const char *call, const char *own);

/* text as cw->code, at 20 WPM or faster to fit BOOT_CW_MAX_MS, cut after the last whole character that fits at
 * 40 WPM. Characters the keyer has no code for are left out. False when nothing in text can be keyed */
bool boot_cw_plan(const char *text, boot_cw_t *cw);

/* in as the own text is kept: capitals, only BOOT_CW_TEXT_CHARS, one space between words and none at the ends, at
 * most BOOT_CW_TEXT_MAX characters. out holds BOOT_CW_TEXT_MAX + 1 bytes. Here, not in boot_cw.c: the settings'
 * validator (cfg/settings_manager.cpp, a library of its own) takes it too */
static inline void boot_cw_clean(const char *in, char *out) {
    size_t n = 0;

    for (; *in && n < BOOT_CW_TEXT_MAX; in++) {
        char c = *in >= 'a' && *in <= 'z' ? (char)(*in - 'a' + 'A') : *in;

        if (!strchr(BOOT_CW_TEXT_CHARS, c)) continue;
        if (c == ' ' && (n == 0 || out[n - 1] == ' ')) continue;
        out[n++] = c;
    }
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
}

#ifdef __cplusplus
}
#endif
