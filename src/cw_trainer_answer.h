/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  CW trainer: how it reads an answer from the decoder (dialog_cw_trainer.c). Shared with its host test
 *  (tools/cw_trainer), which runs these rules against the real decoder.
 */

#pragma once

#include <stddef.h>
#include <string.h>

/* A place of the answer the decoder found no character for: a code not in its table ("<?>": a keying slip, or the
 * group's last element and the answer run together) or a prosign ("<AR>"). Never in a group or a callsign, so never
 * right. One byte, one place: the checks go place by place. It shows as CWT_NO_CHAR_SHOWN. Up to v0.3.8 "<?>" became
 * "?", a Koch character itself (level 32 on): a garbled answer to "?" counted right, and the two looked the same. A
 * prosign went in as its text, one place taking four: the places after it no longer lined up with the group's, and
 * its letters counted for the group's (the R of "<AR>") */
#define CWT_NO_CHAR '*'

/* How a place with no character shows in Learn and Calls: as the CW decoder panel (and Practice) shows a code not in
 * its table. A "?" heard as "?" shows as "?" */
#define CWT_NO_CHAR_SHOWN "<?>"

/* A character from the decoder (not a space) onto the answer heard[] of size bytes: one place each */
static inline void cwt_answer_add(char *heard, size_t size, const char *t) {
    size_t n = strlen(heard);

    if (n + 1 < size) {
        heard[n]     = t[0] && !t[1] ? t[0] : CWT_NO_CHAR;
        heard[n + 1] = 0;
    }
}

/* A place of the answer as shown: CWT_NO_CHAR as CWT_NO_CHAR_SHOWN, any other character as it is (in one) */
static inline const char *cwt_place_shown(char c, char one[2]) {
    if (c == CWT_NO_CHAR) return CWT_NO_CHAR_SHOWN;
    one[0] = c;
    one[1] = 0;
    return one;
}

/* The answer heard[] as shown, into out of size bytes (3 bytes a place is room for any answer); whole places only */
static inline const char *cwt_answer_shown(const char *heard, char *out, size_t size) {
    char   one[2];
    size_t n = 0;

    for (; *heard; heard++) {
        const char *s = cwt_place_shown(*heard, one);
        size_t      l = strlen(s);

        if (n + l >= size) break;
        memcpy(out + n, s, l);
        n += l;
    }
    out[n] = 0;
    return out;
}

#define CWT_ANSWER_MS     3000  /* silence that ends an answer shorter than the group, at least */
#define CWT_ANSWER_LAG_MS 500

/* An answer shorter than the group ends after this long without a character. The decoder gives a character out when
 * the next one starts, the last one 8.5 dots after its end: from one to the next takes up to the longest Koch
 * character, 0 (five dahs of KEY Ratio and four gaps: 19 dots at Ratio 3, 26.5 at 4.5), and those 8.5 dots, more
 * with a straight key's wider gaps. Waited: 30 dots at Ratio 3 and lower, 37.5 at 4.5 (2.5 to spare; a straight
 * key's dahs are the user's own, so never less than at 3), in dots of the slower of the key speed (the group's, a
 * paddle's answer) and the decoder's (a straight key slower than that; 0 not known yet), the audio's hand-over on
 * top. A fixed 3 s cut the answer's last character at 10 WPM (3.3 s), and the answer counted wrong; 30 dots at
 * Ratio 4.5 cut a 0 */
static inline unsigned cwt_answer_ms(int key_wpm, int dec_wpm, float ratio) {
    int      wpm  = dec_wpm > 0 && dec_wpm < key_wpm ? dec_wpm : key_wpm;
    float    dots = 5.0f * (ratio > 3.0f ? ratio : 3.0f) + 15.0f;
    unsigned ms   = wpm > 0 ? (unsigned)(dots * 1200 / wpm) + CWT_ANSWER_LAG_MS : 0;

    return ms > CWT_ANSWER_MS ? ms : CWT_ANSWER_MS;
}
