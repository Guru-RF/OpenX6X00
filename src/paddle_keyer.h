/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  A paddle on the keyboard's [ and ] keys (the N6ARA TinyMIDI in Keyboard Mode sends them) or
 *  on other USB inputs (keys, mouse buttons, MIDI notes, serial lines; Settings > Keys > PADDLE):
 *  in CW / CW-R they key the radio through an iambic keyer that follows the KEY settings.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PADDLE_NONE = 0,
    PADDLE_DIT,
    PADDLE_DAH
} paddle_el_t;

typedef struct {
    bool        mode_b;
    bool        dit_mem, dah_mem;   /* the other paddle touched during an element */
    bool        squeeze;            /* both held during the element */
    paddle_el_t last;               /* the element on (with its space), NONE when idle */
} iambic_t;

/* The paddles as they are now, while the last element and its space are on */
void iambic_paddles(iambic_t *k, bool dit, bool dah);

/* The element after the last one and its space: NONE, the keyer stops */
paddle_el_t iambic_next(iambic_t *k, bool dit, bool dah);

/* Where a paddle press comes from. Each has a map in the settings (p_paddle_kbd...):
 * the left paddle's code in the low 16 bits, the right one's above */
typedef enum {
    PADDLE_SRC_KBD = 0,     /* key codes; [ and ] are always a paddle too */
    PADDLE_SRC_BTN,         /* mouse button codes (BTN_LEFT...) */
    PADDLE_SRC_MIDI,        /* MIDI note numbers */
    PADDLE_SRC_SER,         /* serial modem lines (TIOCM_CTS...) */
    PADDLE_SRC_N
} paddle_src_t;

/* The paddle a code is with this map: 1 left, 2 right, 0 none. Swap exchanges them */
int paddle_side(paddle_src_t src, int code, int map, bool swap);

/* The map for a learned pair: what was pressed for DIT keys a dit, also with the paddles
 * flipped (KEY mode Auto-R or Swap) */
int paddle_learn_map(int dit, int dah, bool flip);

/* MIDI bytes in, one at a time: true when a note on or off is complete */
typedef struct {
    unsigned char status, n, data[2];
} paddle_midi_t;

bool paddle_midi(paddle_midi_t *m, unsigned char byte, int *note, bool *on);

/* Mouse buttons that are a paddle, not a pointer: no X axis, or with one a keyer adapter (adapter: the USB
 * device has a serial port or MIDI too, as -Yuan- 3key, Vail) or the HOTSPOTEK paddle 1500:3001. A real
 * mouse is never a paddle: in CW it would key the radio */
bool paddle_mouse(bool btn_left, bool btn_right, bool rel_x, bool adapter, unsigned vendor, unsigned product);

/* The keyboard just opened (keyboard.c): the keyer reads its own copy of the events */
void paddle_keyer_open(const char *path);

/* Looks for paddles and opens new ones (keyboard.c, on USB changes): a mouse-button paddle and
 * a second USB/Bluetooth keyboard with the paddle keys in /dev/input, a USB MIDI port, and with
 * p_paddle_serial on the first /dev/ttyUSB* or /dev/ttyACM* (never the radio's own ttyS*) */
void paddle_keyer_scan(void);

/* What is open, 1 << PADDLE_SRC_* bits; the serial port's name in tty (may be NULL) */
unsigned paddle_keyer_sources(const char **tty);

/* Learn: presses stop keying and are kept, (src << 16 | code); -1: nothing yet */
void paddle_keyer_learn(bool on);
int  paddle_keyer_learned(void);

/* CW / CW-R and a keyboard paddle key ([ ] or the learned ones): not a key for the UI */
bool paddle_keyer_key(int code);

/* SWR protection tripped: the key opens and the paddle is ignored until both are let go */
void paddle_keyer_stop(void);

/* CW / CW-R: [ and ] are the paddle, not keys */
bool paddle_keyer_active(void);

#ifdef __cplusplus
}
#endif
