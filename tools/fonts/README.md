# Fonts

`bdf2lvgl.py` turns a BDF bitmap font into an LVGL 8 font at 1 bpp, integer-scaled (`--scale`),
for the 1bit theme's pixel family (`src/fonts/pixel_*.c`, from Terminus Font 4.49.1, OFL-1.1;
the licence is `src/fonts/LICENSE-Terminus-OFL-1.1.txt`). `mkpixel.sh` makes all eleven files from
the unpacked Terminus tarball; the command that made each file is in its header. `--check` also
writes a PNG of a test line drawn from the generated data, to look at before committing.
