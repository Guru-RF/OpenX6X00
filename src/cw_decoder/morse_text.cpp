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

void MorseText::emit(const char *text, bool own) {
    if (out_)
        out_(text, own);
}

void MorseText::flush() {
    if (len_ > 0) {
        code_[len_] = '\0';
        const char *t = too_long_ ? nullptr : morse_to_text(code_);
        emit(t ? t : "<?>", own_code_);
        space_ = false;
    }
    len_      = 0;
    too_long_ = false;
    own_code_ = false;
}

void MorseText::put(Sym s) {
    switch (s) {
        case SYM_DIT:
        case SYM_DAH:
            if (len_ < MAX_CODE)
                code_[len_++] = s == SYM_DIT ? '.' : '-';
            else
                too_long_ = true;
            if (own_left_ > 0) {
                own_ended_ = --own_left_ == 0;
                own_run_   = true;
                own_code_  = true;
            }
            break;

        case SYM_CHAR_GAP:
            flush();
            break;

        case SYM_WORD_GAP:
            flush();
            if (own_run_)
                own(0);
            if (!space_) {
                emit(" ", false);
                space_ = true;
            }
            break;

        default:
            break;
    }
}

} // namespace cw
