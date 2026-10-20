/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - Field Ops: parks and summits near a position
 */

#include "refs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static double km_between(double lat1, double lon1, double lat2, double lon2) {
    double p1 = lat1 * M_PI / 180, p2 = lat2 * M_PI / 180;
    double dp = p2 - p1, dl = (lon2 - lon1) * M_PI / 180;
    double a  = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);

    return 6371.0 * 2 * atan2(sqrt(a), sqrt(1 - a));
}

/* The fonts have ASCII only: accented Latin letters (U+00C0..U+017F) lose the accent, others become '?' */
static void ascii_fold(char *dst, size_t size, const char *s) {
    static const char tab[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuytyAaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
    size_t            n     = 0;

    while (*s && n + 1 < size) {
        unsigned char c = *s;
        if (c < 0x80) {
            dst[n++] = *s++;
            continue;
        }
        int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        int cp  = len == 2 && (s[1] & 0xC0) == 0x80 ? ((c & 0x1F) << 6) | (s[1] & 0x3F) : 0;
        dst[n++] = cp >= 0xC0 && cp < 0x180 ? tab[cp - 0xC0] : '?';
        for (int i = 0; i < len && *s; i++)
            s++;
    }
    dst[n] = 0;
}

bool field_ref_parse(char *line, field_ref_t *r) {
    char *f[7];
    int   n = 0;

    for (char *p = line; n < 7;) {
        f[n++] = p;
        p      = strchr(p, '\t');
        if (!p)
            break;
        *p++ = 0;
    }
    if (n < 5 || !f[0][0])
        return false;
    f[n - 1][strcspn(f[n - 1], "\r\n")] = 0;
    r->type = f[0][0];
    snprintf(r->ref, sizeof(r->ref), "%s", f[1]);
    ascii_fold(r->name, sizeof(r->name), f[2]);
    r->lat    = atof(f[3]);
    r->lon    = atof(f[4]);
    r->alt_m  = n > 5 ? atoi(f[5]) : 0;
    r->points = n > 6 ? atoi(f[6]) : 0;
    r->km     = 0;
    return true;
}

int field_refs_near(const char *path, char type, double lat, double lon, field_ref_t *out, int max) {
    FILE *f = fopen(path, "r");
    char  line[256];
    int   n = 0;

    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        field_ref_t r;

        if (line[0] != type || !field_ref_parse(line, &r))
            continue;
        r.km = km_between(lat, lon, r.lat, r.lon);
        if (n == max && r.km >= out[n - 1].km)
            continue;
        /* Insertion in the sorted list, the farthest drops out when full */
        int k = n < max ? n++ : n - 1;
        while (k > 0 && out[k - 1].km > r.km) {
            out[k] = out[k - 1];
            k--;
        }
        out[k] = r;
    }
    fclose(f);
    return n;
}

bool field_refs_find(const char *path, const char *ref, field_ref_t *out) {
    FILE *f = fopen(path, "r");
    char  line[256];
    bool  found = false;

    if (!f)
        return false;
    while (!found && fgets(line, sizeof(line), f))
        found = field_ref_parse(line, out) && !strcasecmp(out->ref, ref);
    fclose(f);
    return found;
}
