/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  CW trainer: how it reads an answer from the decoder (dialog_cw_trainer.c). Shared with its host test
 *  (tools/cw_trainer), which runs these rules against the real decoder.
 */

#pragma once

#define CWT_ANSWER_MS     3000  /* silence that ends an answer shorter than the group, at least */
#define CWT_ANSWER_DOTS   30
#define CWT_ANSWER_LAG_MS 500

/* An answer shorter than the group ends after this long without a character. The decoder gives a character out when
 * the next one starts, the last one 8.5 dots after its end: from one to the next takes up to the longest Koch
 * character (0 and ",", 19 dots) and those 8.5 dots, more with a straight key's wider gaps. A fixed 3 s cut the
 * answer's last character at 10 WPM (3.3 s), and the answer counted wrong. In dots of the slower of the key speed
 * (the group's, a paddle's answer) and the decoder's (a straight key slower than that; 0 not known yet), the
 * audio's hand-over on top */
static inline unsigned cwt_answer_ms(int key_wpm, int dec_wpm) {
    int      wpm = dec_wpm > 0 && dec_wpm < key_wpm ? dec_wpm : key_wpm;
    unsigned ms  = wpm > 0 ? (unsigned)(CWT_ANSWER_DOTS * 1200 / wpm + CWT_ANSWER_LAG_MS) : 0;

    return ms > CWT_ANSWER_MS ? ms : CWT_ANSWER_MS;
}
