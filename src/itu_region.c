/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The ITU region of a position (itu_region.h). Region 2 lies east of Line C and west of Line B, Region 1 east of
 *  Line B and west of Line A, Region 3 east of Line A and west of Line C; then 5.5 puts Armenia, Azerbaijan,
 *  Georgia, Kazakhstan, Kyrgyzstan, Mongolia, Russia, Tajikistan, Turkey, Turkmenistan, Ukraine and Uzbekistan, and
 *  the Arctic north of Russia between Lines A and C, in Region 1, and 5.9 all of Iran in Region 3. The lines are
 *  polylines of (lat, lon) from north to south, their great circles in 16 steps each.
 *
 *  tools/itu_region checks it with 132 places: islands, border towns, both sides of each line.
 */

#include "itu_region.h"

#include <math.h>
#include <stdbool.h>

typedef struct {
    float lat, lon;
} ll_t;

#define LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* Line A: down 40 E to 40 N, the great circle to 60 E on the Tropic of Cancer, down 60 E */
static const ll_t line_a[] = {
    {  90.00,   40.00}, {  40.00,   40.00}, {  39.07,   41.49}, {  38.12,   42.94}, {  37.16,   44.35}, {  36.18,   45.73},
    {  35.18,   47.07}, {  34.17,   48.38}, {  33.14,   49.66}, {  32.11,   50.91}, {  31.06,   52.13}, {  30.00,   53.32},
    {  28.93,   54.49}, {  27.84,   55.64}, {  26.75,   56.76}, {  25.66,   57.86}, {  24.55,   58.94}, {  23.43,   60.00},
    { -90.00,   60.00},
};

/* Line B: down 10 W to 72 N, great circles to 50 W 40 N and to 20 W 10 S, down 20 W */
static const ll_t line_b[] = {
    {  90.00,  -10.00}, {  72.00,  -10.00}, {  70.53,  -15.70}, {  68.90,  -20.58}, {  67.15,  -24.78}, {  65.31,  -28.39},
    {  63.39,  -31.52}, {  61.42,  -34.26}, {  59.39,  -36.66}, {  57.32,  -38.80}, {  55.23,  -40.70}, {  53.10,  -42.41},
    {  50.96,  -43.96}, {  48.79,  -45.37}, {  46.61,  -46.67}, {  44.42,  -47.86}, {  42.22,  -48.97}, {  40.00,  -50.00},
    {  37.07,  -47.38}, {  34.09,  -44.95}, {  31.07,  -42.69}, {  28.00,  -40.57}, {  24.91,  -38.57}, {  21.79,  -36.66},
    {  18.65,  -34.84}, {  15.50,  -33.08}, {  12.33,  -31.38}, {   9.15,  -29.72}, {   5.96,  -28.08}, {   2.77,  -26.47},
    {  -0.43,  -24.86}, {  -3.62,  -23.26}, {  -6.81,  -21.64}, { -10.00,  -20.00}, { -90.00,  -20.00},
};

/* Line C: down the Bering Strait (168 58'37" W) to 65 30' N, great circles to 165 E 50 N and to 170 W 10 N; from
 * there along 10 N to 120 W and down 120 W (lon_c()) */
static const ll_t line_c[] = {
    {  90.00, -168.98}, {  65.50, -168.98}, {  64.72, -171.39}, {  63.90, -173.67}, {  63.05, -175.82}, {  62.17, -177.84},
    {  61.26, -179.75}, {  60.32,  178.46}, {  59.37,  176.76}, {  58.39,  175.16}, {  57.39,  173.64}, {  56.37,  172.21},
    {  55.34,  170.85}, {  54.30,  169.56}, {  53.24,  168.34}, {  52.17,  167.17}, {  51.09,  166.06}, {  50.00,  165.00},
    {  47.70,  167.46}, {  45.35,  169.71}, {  42.95,  171.78}, {  40.53,  173.69}, {  38.07,  175.47}, {  35.59,  177.13},
    {  33.09,  178.69}, {  30.57, -179.83}, {  28.04, -178.43}, {  25.49, -177.10}, {  22.93, -175.82}, {  20.35, -174.59},
    {  17.77, -173.39}, {  15.19, -172.24}, {  12.60, -171.11}, {  10.00, -170.00},
};

/* Region 1 east of Line A: its southern edge from 40 E to past Line C (east longitudes, to 191.5), closed over the
 * pole. The borders within a degree */
static const ll_t r1_east[] = {
    {  36.80,   40.00}, {  37.10,   42.40}, {  37.15,   44.75}, {  39.65,   44.80}, {  38.95,   45.63}, {  38.85,   46.10},
    {  38.87,   46.55}, {  39.65,   47.92}, {  39.40,   48.35}, {  38.85,   48.05}, {  38.43,   48.87}, {  38.43,   49.50},
    {  37.35,   53.90}, {  37.30,   54.00}, {  37.40,   55.50}, {  38.10,   57.30}, {  37.75,   58.40}, {  37.40,   59.50},
    {  36.60,   61.15}, {  35.60,   61.27}, {  35.14,   62.30}, {  35.60,   63.10}, {  36.00,   64.00}, {  36.90,   65.50},
    {  37.30,   66.50}, {  37.20,   67.30}, {  37.15,   67.80}, {  37.00,   68.30}, {  37.30,   69.50}, {  37.60,   70.50},
    {  38.40,   71.10}, {  37.50,   71.50}, {  36.95,   72.30}, {  37.10,   74.00}, {  37.00,   74.90}, {  38.50,   75.10},
    {  39.35,   73.65}, {  39.70,   73.90}, {  40.20,   74.90}, {  40.50,   75.60}, {  40.95,   76.70}, {  41.40,   78.40},
    {  42.20,   80.20}, {  42.90,   80.20}, {  44.20,   80.30}, {  45.20,   82.50}, {  46.70,   83.00}, {  47.20,   85.60},
    {  49.10,   87.30}, {  49.15,   87.80}, {  48.50,   88.50}, {  47.80,   90.00}, {  46.00,   91.00}, {  45.40,   93.00},
    {  44.90,   94.50}, {  44.30,   95.50}, {  42.80,   96.50}, {  42.60,  100.00}, {  42.00,  103.00}, {  41.60,  105.00},
    {  42.30,  107.00}, {  42.50,  109.00}, {  43.50,  111.00}, {  43.70,  112.00}, {  44.60,  113.60}, {  45.40,  116.00},
    {  46.40,  117.30}, {  46.70,  119.90}, {  47.60,  119.70}, {  48.00,  118.50}, {  49.85,  116.70}, {  50.30,  117.80},
    {  51.30,  119.10}, {  52.50,  120.60}, {  53.33,  121.40}, {  53.50,  123.50}, {  53.00,  125.00}, {  52.20,  126.30},
    {  50.25,  127.50}, {  49.50,  129.50}, {  48.00,  130.90}, {  47.70,  132.50}, {  48.40,  134.70}, {  47.60,  134.60},
    {  46.70,  134.00}, {  45.60,  133.20}, {  45.00,  132.40}, {  44.40,  131.20}, {  43.40,  131.25}, {  42.45,  130.60},
    {  42.00,  131.50}, {  44.20,  139.00}, {  45.80,  140.80}, {  45.60,  141.90}, {  44.20,  145.40}, {  43.50,  145.80},
    {  43.20,  146.50}, {  44.00,  157.00}, {  44.00,  191.50}, {  90.00,  191.50}, {  90.00,   40.00},
};

/* Iran, a rough outline */
static const ll_t iran[] = {
    {  39.65,   44.80}, {  38.95,   45.63}, {  38.85,   46.10}, {  38.87,   46.55}, {  39.65,   47.92}, {  39.40,   48.35},
    {  38.85,   48.05}, {  38.43,   48.87}, {  37.40,   49.50}, {  36.70,   51.50}, {  36.90,   53.90}, {  37.35,   53.90},
    {  37.30,   54.00}, {  37.40,   55.50}, {  38.10,   57.30}, {  37.75,   58.40}, {  37.40,   59.50}, {  36.60,   61.15},
    {  35.60,   61.27}, {  34.60,   61.00}, {  31.30,   61.80}, {  29.86,   60.87}, {  27.20,   63.30}, {  25.20,   61.60},
    {  25.50,   59.00}, {  25.70,   57.30}, {  27.10,   56.50}, {  26.50,   54.50}, {  27.60,   52.00}, {  28.90,   50.80},
    {  29.85,   48.60}, {  30.45,   48.03}, {  31.00,   47.70}, {  32.00,   47.40}, {  33.00,   46.20}, {  34.50,   45.50},
    {  35.80,   46.00}, {  37.15,   44.75}, {  38.30,   44.30}, {  39.40,   44.40},
};

/* The longitude of a line at lat, straight between its two points around it; across the date line too */
static double line_lon(const ll_t *line, int n, double lat) {
    for (int i = 0; i + 1 < n; i++) {
        double la1 = line[i].lat, la2 = line[i + 1].lat, lo1 = line[i].lon, lo2 = line[i + 1].lon;

        if (lat > la1 || lat < la2) continue;
        if (la1 == la2) return lo1;
        if (lo2 - lo1 > 180) lo2 -= 360;
        if (lo1 - lo2 > 180) lo2 += 360;
        return lo1 + (la1 - lat) / (la1 - la2) * (lo2 - lo1);
    }
    return line[n - 1].lon;
}

/* Line C ends at 10 N: below it, 120 W */
static double lon_c(double lat) {
    return lat < 10 ? -120 : line_lon(line_c, LEN(line_c), lat);
}

/* Degrees east of Line C, 0 to 360 */
static double east_of(double lon, double c) {
    double d = fmod(lon - c, 360);

    return d < 0 ? d + 360 : d;
}

static bool in_poly(double lat, double lon, const ll_t *p, int n) {
    bool in = false;

    for (int i = 0; i < n; i++) {
        const ll_t *a = &p[i], *b = &p[(i + 1) % n];

        if ((a->lat > lat) != (b->lat > lat)) {
            double x = a->lon + (lat - a->lat) * (b->lon - a->lon) / (b->lat - a->lat);

            if (lon < x) in = !in;
        }
    }
    return in;
}

int itu_region(double lat, double lon) {
    if (!isfinite(lat) || !isfinite(lon)) return 1;
    lat = lat > 90 ? 90 : lat < -90 ? -90 : lat;
    lon = east_of(lon, -180) - 180;     /* -180 .. 180 */

    /* Going east from Line C: Region 2 up to Line B, Region 1 up to Line A, then Region 3 */
    double c = lon_c(lat);
    double d = east_of(lon, c);
    int    r = d < east_of(line_lon(line_b, LEN(line_b), lat), c) ? 2
             : d < east_of(line_lon(line_a, LEN(line_a), lat), c) ? 1 : 3;

    if (r == 3 && in_poly(lat, lon < 0 ? lon + 360 : lon, r1_east, LEN(r1_east))) r = 1;
    if (r != 2 && in_poly(lat, lon, iran, LEN(iran))) r = 3;
    return r;
}
