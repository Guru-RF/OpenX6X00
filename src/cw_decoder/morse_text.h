/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW receiver: dits, dahs and gaps to text.
 */

#pragma once

#include <functional>
#include <utility>

#include "rx_defs.h"

namespace cw {

/* Collects the elements of a character and looks it up at the character gap.
 * One call of the output per character, prosign ("<AR>") or space (the CW
 * trainer counts calls); a code not in the table comes out as "<?>". Never
 * two spaces in a row. own: the character holds one of our own dits and dahs
 * (own()), the CW trainer's group, not the user's answer; also with elements
 * past our count in it: right after the lock has found the tone a dah of ours
 * can come in pieces, the count then runs out inside our last character, and
 * its rest was taken for the answer. An answer keyed within 1.7 dots of our
 * last element runs into it the same way and goes with it (a reaction that
 * quick is rare): the user keys it again. */
class MorseText {
  public:
    using Out = std::function<void(const char *text, bool own)>;

    explicit MorseText(Out out) : out_(std::move(out)) {}

    void put(Sym s);

    /* The next n dits and dahs are our own keying; 0 ends that. The character
     * being collected keeps its mark. The run also ends at the first word gap
     * after one of them: there is none inside a trainer's group and the
     * user's answer mostly comes after one, so a dit or dah of ours the
     * decoder missed does not take one of the answer's */
    void own(int n) {
        own_left_ = n > 0 ? n : 0;
        own_run_  = false;
    }
    /* Our own dits and dahs are still to come */
    bool owning() const { return own_left_ > 0; }
    /* The last of our own dits and dahs came with the last put(): true once */
    bool own_ended() {
        bool e     = own_ended_;
        own_ended_ = false;
        return e;
    }

  private:
    void flush();
    void emit(const char *text, bool own);

    static constexpr size_t MAX_CODE = 12;   /* longer is no character */

    Out    out_;
    char   code_[MAX_CODE + 1] = {};
    size_t len_     = 0;
    bool   too_long_ = false;
    bool   space_   = false;   /* the last output was a space */

    int    own_left_ = 0;      /* own elements still to come */
    bool   own_run_  = false;  /* one of them came */
    bool   own_ended_ = false; /* the last one came (own_ended) */
    bool   own_code_ = false;  /* the character being collected holds one */
};

} // namespace cw
