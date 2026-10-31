/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - the text of the keyboard modes on the card (#55): what PSK31, PSK63, Olivia and JTTY receive and
 *  send, and what RTTY receives, a line each in DATA's digi_text/<UTC date>.txt, one file a day:
 *
 *    2026-10-31 14:05:12Z 14.071000 PSK31 RX Q87% CQ CQ DE ON6URE ON6URE K
 *    2026-10-31 14:05:40Z 14.071000 PSK31 TX - ON6URE DE PA3XYZ PA3XYZ KN
 *    2026-10-31 14:07:02Z 14.090850 JTTY RX -12dB CQ PA3XYZ JO21
 *
 *  The UTC time the line began, the frequency in MHz (the dial and the audio offset: where the signal is), the
 *  mode, RX or TX, the app's own figure for the signal ("Q87%" the decoder's quality as the PSK and Olivia status
 *  buttons show it, "-12dB" JTTY's S/N, averaged over the line; "-" none: RTTY, and what is sent) and the text.
 *
 *  A received line ends at the decoder's line break, after a pause of DIGI_TEXTLOG_PAUSE_S, at a move of the
 *  frequency, at DIGI_TEXTLOG_TEXT_MAX characters, and when the app closes. Lines go to the card as they end, on
 *  a thread of their own (no app waits on the card); the card is made to write them out (fsync) at most every
 *  DIGI_TEXTLOG_SYNC_S, so a power cut loses the last seconds at most and a station received all day costs the
 *  card no more than that.
 *
 *  Settings > General > TEXT LOG: on by default, and a size limit for the folder (10 MB by default): past it, the
 *  oldest day's file goes. The newest day's file is never deleted: when it alone is at the limit, nothing more is
 *  kept until the next UTC day. Nothing is written while DATA is not mounted (the system partition's /mnt).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board.h"

#define DIGI_TEXTLOG_DIR      BOARD_DATA "/digi_text"
#define DIGI_TEXTLOG_PAUSE_S  10        /* nothing new for this long: the line is written */
#define DIGI_TEXTLOG_TEXT_MAX 200       /* characters of text in a line; more go on the next one */
#define DIGI_TEXTLOG_FREQ_HZ  30        /* a received line that moves more than this from its start ends */
#define DIGI_TEXTLOG_SYNC_S   15        /* the card is made to write the lines out at most this often */

#ifdef __cplusplus
extern "C" {
#endif

/* The figure an app gives for the signal: none ("-"), the decoder's quality 0-100 ("Q87%"), S/N in dB ("-12dB") */
typedef enum {
    DIGI_TEXTLOG_SIG_NONE,
    DIGI_TEXTLOG_SIG_Q,
    DIGI_TEXTLOG_SIG_DB,
} digi_textlog_sig_t;

/* Received text as the decoder gives it, a character or a few at a time, any thread: onto the open line, which
 * '\n' or '\r' ends (control characters are dropped, a byte past ASCII is '?'). sig is the app's figure as the text
 * came */
void digi_textlog_rx(const char *mode, uint32_t freq_hz, digi_textlog_sig_t kind, int sig, const char *text);

/* A received message that grows frame by frame (JTTY), any thread: its whole text so far, under the receiver's id.
 * Written once complete, or when it has not grown for DIGI_TEXTLOG_PAUSE_S */
void digi_textlog_msg(const char *mode, uint32_t id, uint32_t freq_hz, digi_textlog_sig_t kind, int sig,
                      const char *text, bool complete);

/* Text sent, any thread: a TX line for each of its lines, after the received line that was open */
void digi_textlog_tx(const char *mode, uint32_t freq_hz, const char *text);

/* The app closes: its open lines are written now */
void digi_textlog_end(void);

/* Tests: wait until the writer has handled everything so far, pauses at the time now included */
void digi_textlog_drain(void);

#ifdef __cplusplus
}
#endif
