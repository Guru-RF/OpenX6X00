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
 * two spaces in a row. */
class MorseText {
  public:
    using Out = std::function<void(const char *)>;

    explicit MorseText(Out out) : out_(std::move(out)) {}

    void put(Sym s);

  private:
    void flush();
    void emit(const char *text);

    static constexpr size_t MAX_CODE = 12;   /* longer is no character */

    Out    out_;
    char   code_[MAX_CODE + 1] = {};
    size_t len_     = 0;
    bool   too_long_ = false;
    bool   space_   = false;   /* the last output was a space */
};

} // namespace cw
