/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the Internet radio's station lines (src/iradio_list.c): the lines rootfs/usr/sbin/x6100-iradio
 *  prints and the favourites file, as the window reads them; which stations play with ffmpeg on the image and
 *  without; the list's second column; the favourites written and read back, a long line and junk skipped.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */
#include "iradio_list.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails, checks;

#define CHECK(cond)                                                       \
    do {                                                                  \
        checks++;                                                         \
        if (!(cond)) {                                                    \
            fails++;                                                      \
            fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

#define STREQ(a, b)                                                                      \
    do {                                                                                 \
        checks++;                                                                        \
        if (strcmp((a), (b))) {                                                          \
            fails++;                                                                     \
            fprintf(stderr, "%s:%d: FAIL \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
        }                                                                                \
    } while (0)

static bool parse(const char *text, iradio_station_t *s) {
    char line[1024];

    snprintf(line, sizeof(line), "%s", text);
    return iradio_parse_station(line, s);
}

static void test_stations(void) {
    iradio_station_t s;
    char             info[48];

    /* A line of the helper's */
    CHECK(parse("Radio Example One\thttp://stream.example.org/one.mp3\tMP3\t128\tBE\t"
                "96062a7b-0601-11e8-ae97-52543be04c81\n", &s));
    STREQ(s.name, "Radio Example One");
    STREQ(s.url, "http://stream.example.org/one.mp3");
    STREQ(s.codec, "MP3");
    STREQ(s.kbps, "128");
    STREQ(s.cc, "BE");
    STREQ(s.uuid, "96062a7b-0601-11e8-ae97-52543be04c81");
    CHECK(iradio_playable(&s, false));
    iradio_info(&s, false, info, sizeof(info), "not playable");
    STREQ(info, "MP3 128k BE");

    /* The web page's line: lower case, CR LF, no uuid; a bad kbps and country dropped */
    CHECK(parse("Jazz\thttps://edge.example.net/jazz\taac+\t12x\tbel\r\n", &s));
    STREQ(s.codec, "AAC+");
    STREQ(s.kbps, "");
    STREQ(s.cc, "");
    STREQ(s.uuid, "");
    CHECK(!iradio_playable(&s, false));
    CHECK(iradio_playable(&s, true));
    iradio_info(&s, false, info, sizeof(info), "not playable");
    STREQ(info, "AAC+ not playable");
    iradio_info(&s, true, info, sizeof(info), "not playable");
    STREQ(info, "AAC+");

    /* An address typed in: name and address only, its codec not known, so it is tried */
    CHECK(parse("stream.example.org\thttp://stream.example.org:8000/live", &s));
    STREQ(s.codec, "");
    CHECK(iradio_playable(&s, false));
    iradio_info(&s, false, info, sizeof(info), "not playable");
    STREQ(info, "");

    /* Refused: no name, no http(s), a space or a quote in the address, one field, an address too long */
    CHECK(!parse("\thttp://x/", &s));
    CHECK(!parse("Name\tftp://x/s.mp3", &s));
    CHECK(!parse("Name\thttp://x/a b", &s));
    CHECK(!parse("Name\thttp://x/a\"b", &s));
    CHECK(!parse("Name only", &s));
    char longline[600];
    snprintf(longline, sizeof(longline), "Long\thttp://x/%0300d", 0);
    CHECK(!parse(longline, &s));

    /* A long name cut on a whole character: 70 bytes of two-byte letters into 63 */
    char name[200] = "";
    for (int i = 0; i < 35; i++) strcat(name, "\xc3\xa9");
    char line[300];
    snprintf(line, sizeof(line), "%s\thttp://x/", name);
    CHECK(parse(line, &s));
    CHECK(strlen(s.name) == 62);
    CHECK(((unsigned char)s.name[61] & 0xc0) == 0x80);  /* ends on the second byte of a letter */

    /* A codec too long for its field is cut, an odd uuid dropped */
    CHECK(parse("N\thttp://x/\tVERYLONGCODEC\t64\tUY\tnot-a-uuid", &s));
    STREQ(s.codec, "VERYLONG");
    STREQ(s.cc, "UY");
    STREQ(s.uuid, "");
}

static void test_countries(void) {
    iradio_country_t c;
    char             line[200];

    snprintf(line, sizeof(line), "Belgium\tBE\t412\n");
    CHECK(iradio_parse_country(line, &c));
    STREQ(c.name, "Belgium");
    STREQ(c.cc, "BE");
    CHECK(c.stations == 412);
    snprintf(line, sizeof(line), "Number#Sign\tNS");
    CHECK(iradio_parse_country(line, &c));
    STREQ(c.name, "Number Sign");
    CHECK(c.stations == 0);
    snprintf(line, sizeof(line), "Lower\tbe\t3");
    CHECK(!iradio_parse_country(line, &c));
    snprintf(line, sizeof(line), "Three\tBEL\t3");
    CHECK(!iradio_parse_country(line, &c));
    snprintf(line, sizeof(line), "error no answer from radio-browser.info");
    CHECK(!iradio_parse_country(line, &c));
}

static void test_file(void) {
    char dir[] = "/tmp/iradio-test-XXXXXX";
    char path[200], tmp[300];

    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        exit(2);
    }
    snprintf(path, sizeof(path), "%s/iradio/favorites.tsv", dir);   /* the folder made by the save */

    iradio_station_t list[3], back[10];
    CHECK(parse("One\thttp://one.example.org/\tMP3\t128\tBE\t96062a7b-0601-11e8-ae97-52543be04c81", &list[0]));
    CHECK(parse("Two\thttps://two.example.org/s\tOGG\t\t\t", &list[1]));
    CHECK(parse("Three\thttp://three.example.org:8000/", &list[2]));
    CHECK(iradio_save(path, list, 3));
    CHECK(iradio_load(path, back, 10) == 3);
    CHECK(!memcmp(list, back, sizeof(list)));
    CHECK(iradio_find(back, 3, "https://two.example.org/s") == 1);
    CHECK(iradio_find(back, 3, "https://two.example.org/") == -1);
    CHECK(iradio_load(path, back, 2) == 2);     /* at most max */
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    CHECK(access(tmp, F_OK) != 0);              /* put in place, nothing left over */

    /* A file written by hand: a line too long for the reader and junk are skipped, the others read */
    FILE *f = fopen(path, "w");
    fprintf(f, "Before\thttp://a.example.org/\n");
    fprintf(f, "Long\thttp://b.example.org/");
    for (int i = 0; i < 700; i++) fputc('x', f);
    fprintf(f, "\njunk line\n\nAfter\thttp://c.example.org/\tAAC\t64\tUY");     /* the last without a line end */
    fclose(f);
    CHECK(iradio_load(path, back, 10) == 2);
    STREQ(back[0].name, "Before");
    STREQ(back[1].name, "After");
    STREQ(back[1].cc, "UY");

    /* None saved: an empty file, read as no favourites */
    CHECK(iradio_save(path, list, 0));
    CHECK(iradio_load(path, back, 10) == 0);
    unlink(path);
    CHECK(iradio_load(path, back, 10) == 0);    /* no file */

    snprintf(tmp, sizeof(tmp), "%s/iradio", dir);
    rmdir(tmp);
    rmdir(dir);
}

int main(void) {
    test_stations();
    test_countries();
    test_file();
    printf("iradio_list: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
