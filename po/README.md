# The GUI's languages

One file per language, gettext's `.po`: `es.po` Spanish, `fr.po` French, `nl.po` Dutch, `zh_CN.po` Simplified
Chinese, and the beginnings `de.po` German and `it.po` Italian. The English is the key: each entry is a text as the
GUI shows it in English (`msgid`) and the same text in the language (`msgstr`). An entry left empty shows in English.
`openx6x00.pot` is the list of all the texts, made from the sources; it is not edited by hand.

The languages in `LINGUAS` go into the image; Settings > Display > Language lists them. A file whose header says
`X-OpenX6X00-Status: draft` shows as "(draft)" in that list.

## Translating

Any editor that knows gettext works (Poedit, Lokalize, Gtranslator, a web tool such as Weblate), or a text editor.
On the radio, without building anything:

1. Copy your `<code>.po` to the radio's DATA partition, into a folder `i18n` (over SSH: `/mnt/i18n/<code>.po`; on
   the card: the folder `i18n` on the DATA partition).
2. Settings > Display > Language: choose it (choosing it again reads the file again). The radio's copy is used
   before the one in the image, so what you see is your file.
3. Send the file back as a pull request or attach it to an issue "Translation: <language>".

A few rules, which `python3 -I tools/i18n/potool.py check` also checks:

- Keep every `%d`, `%s`, `%i`, `%.1f`... in the same order. A translation whose conversions differ is left out by the
  GUI (it would crash it) and the English shows.
- `\n` is a new line. A button has two lines of about ten letters: "Filter low:\n%i Hz" can be
  "Filtre bas:\n%i Hz", not a long sentence on one line.
- Leave the text between `#` signs in its place: `#4f8bf5 30#` is a colour (LVGL), only the words outside the codes
  are text.
- Names stay as they are: the radio's keys (MFK, VOL, APP, PTT), the modes (USB, CW, FT8), the themes (Classic,
  VISION), programs and services (Winlink, Pat, POTA, Wavelog).
- A word may be used on its own and in a longer text. The GUI looks a text up whole, then line by line, then a line
  without its colon, then the label of a "Label: value" line: "Volume" also gives "Volume: 30" and "Volume:\n30".
- An entry marked `#, fuzzy` is not used: remove the mark when the translation is right.

Texts the list misses (made on the fly, or found nowhere by the extractor): run the PC build (`host/build.sh`) with
`X6100_I18N_MISSING=/tmp/missing.txt`, open the screens, and the file lists every text that reached the screen without
its translation. Add those as entries.

## A new language

    python3 -I tools/i18n/potool.py new <code> <name in the language>     e.g.  new pt_BR "Português (Brasil)"

then translate, set `X-OpenX6X00-Status:` to empty when the language is done, and add the code to `LINGUAS`.

A language whose letters our fonts lack (they have Latin-1: the Western European languages) names a TrueType or
OpenType font in its header, `X-OpenX6X00-Font: <file>`, which the GUI puts behind its own fonts and draws as needed.
The file is looked for beside the `.po` (so a translator's font goes in `/mnt/i18n` too) and in
`/usr/share/x6100/fonts`. Chinese uses `NotoSansCJKsc-GB2312.otf` there, cut down to GB2312 by
`tools/fonts/mkcjk.sh`: a character beyond it shows as a box until the font is made again.

## For developers

    python3 -I tools/i18n/potool.py extract    after changing texts in src/: openx6x00.pot again
    python3 -I tools/i18n/potool.py update     each .po brought up to it (a text gone: kept at the end as #~)
    python3 -I tools/i18n/potool.py check      the conversions, and how much of each is done
    make -C tools/i18n test                    the GUI's lookup (src/i18n.cpp) and the check

Most code needs nothing: every text handed to LVGL goes through the language (`src/i18n_hook.h`). A text made with
`snprintf` before it reaches LVGL is looked up whole, then line by line; a format that should be translated before
the numbers are in is marked `_("...")`. Code that reads a text back from a widget gets the translation: compare by
position or by value, not by the shown English (Settings' Theme row takes the name by its place in the list).

The hook and the line-by-line matching come from the CX8ANM edition of OpenX6X00 (condoruy, Cóndor Tecnológico),
whose Spanish, French, German and Italian lists were the first entries of these files.
