#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { FONT_SONY, FONT_ROBOTO, FONT_DEJAVU, FONT_ARIAL, FONT_RCOND, FONT_PIXEL };

/* At boot and on a theme change, before the styles and widgets are made with the fonts */
void fonts_apply(uint8_t choice);
void fonts_scale(int steps);
const struct _lv_font_t *font_down(const struct _lv_font_t *f, int steps);

#ifdef __cplusplus
}
#endif
