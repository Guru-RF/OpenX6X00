/* Host test of src/i18n.cpp: the .po reading, the lookup whole, line by line, around the colons and the colour
 * spans, the checks on a translation, the translator's copy before the image's, the list. Run by `make test`.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF */
#include <stdio.h>
#include <string.h>
#include <string>

#include "i18n.h"

static int failed, checks;

static void check(bool ok, const char *what) {
    checks++;
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        failed++;
}

static bool is(const char *got, const char *want) {
    if (strcmp(got, want) == 0)
        return true;
    printf("       got \"%s\", want \"%s\"\n", got, want);
    return false;
}

int main(void) {
    const char *s = "Settings";

    printf("English\n");
    check(i18n_tr(s) == s && !strcmp(i18n_code(), ""), "no language: the text itself, the same pointer");
    check(i18n_font() == NULL, "no font asked for");

    printf("The translator's copy first\n");
    check(i18n_load("xx"), "xx loads");
    check(is(i18n_tr("Settings"), "Mijn instellingen"), "BOARD_DATA/i18n/xx.po before the image's");
    check(is(i18n_tr("Filter low"), "Filter low"), "the copy is the whole language: the image's file is not read");

    printf("The image's file\n");
    rename("user/xx.po", "user/xx.po.off");
    check(i18n_load("xx"), "xx loads again, from the share directory");
    check(is(i18n_tr("Settings"), "Instellingen"), "a text found whole");
    check(is(i18n_tr("Filter low:\n200 Hz"), "Filter laag:\n200 Hz"), "line by line, the colon kept, a number line as it is");
    check(is(i18n_tr("  Off "), "  Uit "), "the spaces at either end kept");
    check(is(i18n_tr("#cbd5e1 Volume:# #4f8bf5 30#"), "#cbd5e1 Volume X:# #4f8bf5 30#"), "colour spans: the text in them translated, the codes kept");
    check(is(i18n_tr("\xEF\x80\x8C Settings"), "\xEF\x80\x8C Instellingen"), "an LVGL symbol before the text kept");
    check(is(i18n_tr("Off \xEF\x81\x94"), "Uit \xEF\x81\x94"), "and after it");
    check(is(i18n_tr("(Settings\n1:3)"), "(Instellingen\n1:3)"), "a page button: the brackets kept");
    check(is(i18n_tr("Settings: USB"), "Instellingen: USB"), "a \"Label: value\" line by its label");
    check(is(i18n_tr("Volume: 30"), "Volume X: 30"), "the label found with its colon dropped");
    check(is(i18n_tr("Nothing: here"), "Nothing: here"), "an unknown label: as it is");
    check(is(i18n_tr("TX power %0.0f W on battery"), "Zendvermogen %0.0f W op de batterij"), "a format with the same conversion");
    check(is(i18n_tr("%d of %s"), "%d of %s"), "conversions in another order: left out (it would crash a format)");
    check(is(i18n_tr("Fuzzy one"), "Fuzzy one"), "a fuzzy translation is not used");
    check(is(i18n_tr("Mode"), "Mode"), "a text with a context is not taken for the plain text");
    check(is(i18n_tr("Two lines\nat once"), "Twee regels\ntegelijk"), "a text of several quoted lines");
    check(is(i18n_tr("Say \"hi\""), "Zeg \"hoi\"\tnu"), "escapes undone");
    check(is(i18n_tr("Empty"), "Empty"), "an empty translation: the English");
    check(is(i18n_tr("Old one"), "Old one"), "an obsolete entry (#~) is not read");
    check(is(i18n_tr("After the old"), "Na het oude"), "the entry after an obsolete one is");
    check(is(i18n_tr("Sprechen"), "说话"), "UTF-8 as it is");
    const char *num = "14.074.000";
    check(i18n_tr(num) == num, "a text without letters: the same pointer, not looked up");
    check(is(i18n_tr("Unknown words"), "Unknown words"), "a text the file lacks: the English");
    const char *st1 = i18n_tr_static("Filter low:\n100 Hz");
    const char *st2 = i18n_tr_static("Filter low:\n100 Hz");
    check(st1 == st2 && is(st1, "Filter laag:\n100 Hz"), "i18n_tr_static: put together once, the same pointer after");
    std::string many;
    for (int i = 0; i < 40; i++)
        i18n_tr("Filter low:\n1 Hz");
    check(is(i18n_tr("Filter low:\n2 Hz"), "Filter laag:\n2 Hz"), "the ring of texts put together goes round");
    check(i18n_font() && strstr(i18n_font(), "fonts/test.ttf"), "the font the header names, found in the fonts directory");
    rename("user/xx.po.off", "user/xx.po");

    printf("Other languages, the list, bad names\n");
    check(i18n_load("yy") && is(i18n_tr("Settings"), "Y-Settings"), "yy loads");
    check(!i18n_load("zz") && i18n_tr(s) == s && !strcmp(i18n_code(), ""), "no zz.po: false, English");
    check(!i18n_load("../share/xx") && i18n_tr(s) == s, "a name with a path in it is refused");
    check(i18n_load("") && i18n_tr(s) == s, "\"\": English");
    check(i18n_load("en") && i18n_tr(s) == s, "\"en\": English");

    i18n_lang_t langs[8];
    int         n = i18n_list(langs, 8);
    check(n == 3, "English, xx, yy listed once each");
    check(n >= 1 && !langs[0].code[0] && is(langs[0].name, "English"), "English first");
    check(n >= 2 && is(langs[1].code, "xx") && is(langs[1].name, "Testisch (mine)") && langs[1].user,
          "xx from the translator's copy, its name from the header");
    check(n >= 3 && is(langs[2].code, "yy") && is(langs[2].name, "Ypsilon") && !langs[2].user, "yy from the image's");
    check(i18n_list(langs, 1) == 1, "the list stops at max");

    printf("Capitals\n");
    char up[64];
    i18n_upper(up, sizeof(up), "Inalámbrico œuvre ÿ łódź š ß 设置 kHz", true);
    check(is(up, "INALÁMBRICO ŒUVRE Ÿ ŁÓDŹ Š ß 设置 KHZ"), "Latin-1 and Latin Extended-A, another script as it is");
    snprintf(up, sizeof(up), "%s", "Señal 7 kHz");
    i18n_upper(up, sizeof(up), up, false);
    check(is(up, "SeÑal 7 kHz"), "in place, the ASCII left to the caller");
    i18n_upper(up, 5, "abéd", true);
    check(is(up, "ABÉ"), "cut on a whole character");

    printf("%s: %d checks, %d failed\n", failed ? "FAILED" : "ALL OK", checks, failed);
    return failed ? 1 : 0;
}
