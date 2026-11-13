/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  A paddle on the keyboard's [ and ] keys (the N6ARA TinyMIDI in Keyboard Mode sends them) or
 *  on other USB inputs (keys, mouse buttons, MIDI notes, serial lines; Settings > Keys > PADDLE):
 *  in CW / CW-R they key the radio through an iambic keyer that follows the KEY settings. SmartCW's
 *  MIDI keys the radio itself with what its own keyer timed, or hands its paddles to the radio's keyer.
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
    bool        other_on;           /* the other paddle held since the element started: a squeeze going on */
    bool        kept;               /* the squeeze ended with the other paddle, pressed during the element,
                                     * still held: mode A keeps its memory */
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

/* SmartCW, RF.Guru's keyer, sends its MIDI notes on channel 1. These are its defaults (its settings midiStraightNote,
 * midiCwNote, midiPttNote, midiDitNote and midiDahNote); the radio takes these numbers and asks SmartCW for none */
enum {
    SMARTCW_NOTE_STRAIGHT = 64,   /* MIDI mode "keyed": its straight key closed */
    SMARTCW_NOTE_KEYER    = 65,   /* "keyed": an element of its keyer, its text, memories or beacon */
    SMARTCW_NOTE_PTT      = 66,   /* a PTT pulse */
    SMARTCW_NOTE_DIT      = 67,   /* MIDI mode "paddles": the dit contact as it is */
    SMARTCW_NOTE_DAH      = 68    /* "paddles": the dah contact */
};

/* SmartCW's USB IDs: 1209:ff42, ff43 with its USB keyboard on */
bool paddle_smartcw(unsigned vendor, unsigned product);

/* What a note from SmartCW does: keys the radio as it comes (KEYED), a paddle for the keyer here (LEFT, RIGHT, as
 * Swap leaves them), nothing (NONE, its PTT pulse: in CW the key brings the transmitter on), or what any MIDI note
 * does, by the MIDI map (OTHER: other note numbers set on SmartCW) */
typedef enum {
    SMARTCW_OTHER = 0,
    SMARTCW_KEYED,
    SMARTCW_LEFT,
    SMARTCW_RIGHT,
    SMARTCW_NONE
} smartcw_note_t;

smartcw_note_t smartcw_note(int note, bool swap);

/* The USB device a sysfs folder belongs to: from dir (links followed) up to the first folder with idVendor and
 * idProduct. False when there is none (not on USB) */
bool paddle_usb_id(const char *dir, unsigned *vendor, unsigned *product);

/* SmartCW's keyed notes as a straight key: SmartCW timed them, so each note on is an element (or the straight key
 * closed) from its start to its note off. One that starts while busy (another source has the key, or the radio may
 * not key: not CW, a CW message on the air) is dropped whole, up to the note off of every keyed note held: keyed
 * late it would be an element cut short, a wrong character */
typedef struct {
    unsigned char held;   /* the keyed notes held: 1 the keyer's, 2 the straight key's */
    bool          drop;   /* what is held is not keyed */
} smartcw_key_t;

void smartcw_keyed(smartcw_key_t *s, int note, bool on, bool busy);

/* What is held now is not keyed any more (the key went up for another reason): up to the note off */
void smartcw_drop(smartcw_key_t *s);

/* Key down for SmartCW */
bool smartcw_down(const smartcw_key_t *s);

/* Mouse buttons that are a paddle, not a pointer: no X axis (rel_x: relative or absolute, so a touchscreen,
 * a tablet or a KVM's absolute mouse is a pointer too), or with one a keyer adapter (adapter: the USB
 * device has a serial port or MIDI too, as -Yuan- 3key, Vail) or the HOTSPOTEK paddle 1500:3001. A real
 * mouse is never a paddle: in CW it would key the radio */
bool paddle_mouse(bool btn_left, bool btn_right, bool rel_x, bool adapter, unsigned vendor, unsigned product);

/* The keyboard just opened (keyboard.c): the keyer reads its own copy of the events */
void paddle_keyer_open(const char *path);

/* Looks for paddles and opens new ones (keyboard.c, on USB changes): a mouse-button paddle and
 * a second USB/Bluetooth keyboard with the paddle keys in /dev/input, a USB MIDI port, and with
 * p_paddle_serial on the first /dev/ttyUSB* or /dev/ttyACM* (never the radio's own ttyS*, nor SmartCW's) */
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

/* The MIDI port open is SmartCW's */
bool paddle_keyer_smartcw(void);

#ifdef __cplusplus
}
#endif
