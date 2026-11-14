/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Morse code table.
 */

#include <string.h>

#include "morse_code.h"

cw_characters_t cw_characters[] = {
    /* letters */
    { ".-", "A" },      { "-...", "B" },    { "-.-.", "C" },    { "-..", "D" },
    { ".", "E" },       { "..-.", "F" },    { "--.", "G" },     { "....", "H" },
    { "..", "I" },      { ".---", "J" },    { "-.-", "K" },     { ".-..", "L" },
    { "--", "M" },      { "-.", "N" },      { "---", "O" },     { ".--.", "P" },
    { "--.-", "Q" },    { ".-.", "R" },     { "...", "S" },     { "-", "T" },
    { "..-", "U" },     { "...-", "V" },    { ".--", "W" },     { "-..-", "X" },
    { "-.--", "Y" },    { "--..", "Z" },

    /* Letters of other languages (#265), in UTF-8: Å Ä Ö Ü É Ñ. No other entry here has their codes, but some
     * stand for another letter elsewhere: .--.- also for À, ---. for Ó and Ø, .-.- for Æ and for AA, the new-line
     * prosign, which had no entry (it came out as <?>). One text a code: the letter of the languages that send it
     * most (Swedish, Finnish, German), the one asked for. The keyer keys each as its code. The GUI's fonts have
     * these six from tools/fonts/mklatin.sh: a letter added here goes into its range too. CH is one sign for two
     * letters: after the letters, so the keyer takes a typed "CH" as C and H, as an English text needs it; only the
     * decoder shows it */
    { ".--.-", "\xc3\x85" },  { ".-.-", "\xc3\x84" },   { "---.", "\xc3\x96" },   { "..--", "\xc3\x9c" },
    { "..-..", "\xc3\x89" },  { "--.--", "\xc3\x91" },  { "----", "CH" },

    /* figures */
    { ".----", "1" },   { "..---", "2" },   { "...--", "3" },   { "....-", "4" },
    { ".....", "5" },   { "-....", "6" },   { "--...", "7" },   { "---..", "8" },
    { "----.", "9" },   { "-----", "0" },

    /* punctuation; "!" has two codes in use, the keyer sends the first */
    { "-.-.--", "!" },  { "..--.", "!" },   { ".-..-.", "\"" }, { "...-..-", "$" },
    { ".----.", "'" },  { "--..--", "," },  { "-....-", "-" },  { ".-.-.-", "." },
    { "-..-.", "/" },   { "---...", ":" },  { "-.-.-.", ";" },  { "-...-", "=" },
    { "..--..", "?" },  { ".--.-.", "@" },  { "..--.-", "_" },

    /* prosigns */
    { ".-.-.", "<AR>" },        { ".-...", "<AS>" },        { "-...-.-", "<BK>" },
    { "-.-..-..", "<CL>" },     { "-.-.-", "<CT>" },        { "-.--.", "<KN>" },
    { "...-.-", "<SK>" },       { "...-.", "<SN>" },        { "...---...", "<SOS>" },
    { "-.-.--.-", "<CQ>" },

    /* a run of dots is the sender's error sign */
    { "......", "<ERR>" },      { ".......", "<ERR>" },     { "........", "<ERR>" },

    { NULL, NULL }
};

/* Byte i of s in capitals: ASCII's small letters, and Latin-1's (U+00E0..U+00FE but U+00F7), whose second byte
 * after 0xC3 is the capital's plus 0x20 ("\xc3\xa5" is "\xc3\x85", Å). 0xC3 is never a second byte */
static unsigned char upper_at(const unsigned char *s, size_t i) {
    unsigned char c = s[i];

    if (c >= 'a' && c <= 'z')
        return c - ('a' - 'A');
    if (i > 0 && s[i - 1] == 0xC3 && c >= 0xA0 && c <= 0xBE && c != 0xB7)
        return c - 0x20;
    return c;
}

const cw_characters_t *morse_find(const char *text) {
    for (const cw_characters_t *c = cw_characters; c->morse; c++) {
        size_t i = 0;

        while (c->character[i] && upper_at((const unsigned char *)text, i) == (unsigned char)c->character[i])
            i++;
        if (!c->character[i])
            return c;
    }
    return NULL;
}

const char *morse_to_text(const char *code) {
    for (const cw_characters_t *c = cw_characters; c->morse; c++)
        if (strcmp(c->morse, code) == 0)
            return c->character;
    return NULL;
}
