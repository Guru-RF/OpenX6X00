# Fonts

`bdf2lvgl.py` turns a BDF bitmap font into an LVGL 8 font at 1 bpp, integer-scaled (`--scale`),
for the 1bit theme's pixel family (`src/fonts/pixel_*.c`, from Terminus Font 4.49.1, OFL-1.1;
the licence is `src/fonts/LICENSE-Terminus-OFL-1.1.txt`). `mkpixel.sh` makes all eleven files from
the unpacked Terminus tarball; the command that made each file is in its header. `--check` also
writes a PNG of a test line drawn from the generated data, to look at before committing.

`mklatin.sh` makes `src/fonts/latin1_*.c`: the capitals the CW decoder and the CW Keyboard show
beyond ASCII, Å Ä Ö Ü É Ñ (#265), and their small letters å ä ö ü é ñ typed into the text boxes (#269),
from DejaVu Sans at each dejavu_* size, with lv_font_conv 1.5.3.
Those twelve only: a letter added to the Morse table goes into its `--range` too, both cases.
The other families are ASCII (the pixel family has Latin-1 of its own); `src/fonts/font_select.c`
puts one of these behind each font, the size whose capitals match the font's own. They are not added to the fonts themselves: lv_font_conv
would raise each font's line height to the accented capitals' and every layout would move.
