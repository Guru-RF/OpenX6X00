/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW receiver: Morse to text.
 */

#include "morse_text.h"

#include "morse_code.h"

namespace cw {

void MorseText::emit(const char *text) {
    if (out_)
        out_(text);
}

void MorseText::flush() {
    if (len_ > 0) {
        code_[len_] = '\0';
        const char *t = too_long_ ? nullptr : morse_to_text(code_);
        emit(t ? t : "<?>");
        space_ = false;
    }
    len_      = 0;
    too_long_ = false;
}

void MorseText::put(Sym s) {
    switch (s) {
        case SYM_DIT:
        case SYM_DAH:
            if (len_ < MAX_CODE)
                code_[len_++] = s == SYM_DIT ? '.' : '-';
            else
                too_long_ = true;
            break;

        case SYM_CHAR_GAP:
            flush();
            break;

        case SYM_WORD_GAP:
            flush();
            if (!space_) {
                emit(" ");
                space_ = true;
            }
            break;

        default:
            break;
    }
}

} // namespace cw
