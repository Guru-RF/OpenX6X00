/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/itu_region.c: 132 places whose ITU region is known, at their own coordinates, then through
 *  the centres of their 6-character and 4-character locators (Settings > General > QTH locator). Islands near the
 *  lines (Iceland R1 and Greenland R2 either side of Line B in the Denmark Strait, the Azores, Bermuda, St Peter and
 *  St Paul Rocks), the Bering Strait (Little Diomede R2, Big Diomede R1), the Pacific places Appendix 1 of 47 CFR
 *  Part 97 names (Midway and Johnston R2; Wake, Guam, Palmyra, Kingman, Jarvis, Baker and American Samoa R3),
 *  Sakhalin and the Kurils R1 against Hokkaido R3, the Amur, Mongolia and its Chinese neighbours, Central Asia, all
 *  of Iran R3 against Iraq, the Gulf and Oman R1, eastern Turkey and the Caucasus R1.
 *
 *  A place wrong at its coordinates or at its 6-character locator fails the test. A 4-character square is 2 by 1
 *  degrees: where a border runs through one, its centre can be on the other side. Those are listed, not counted
 *  (the manual says a 6-character locator or the GPS sets them right).
 */

#include "itu_region.h"
#include "qth/qth.h"

#include <stdio.h>

static const struct {
    const char *name;
    double      lat, lon;
    int         want;
} places[] = {
    {"Brussels", 50.85, 4.35, 1}, {"Reykjavik", 64.15, -21.94, 1}, {"Iceland W tip", 65.50, -24.50, 1},
    {"Tasiilaq GL", 65.60, -37.60, 2}, {"Nuuk GL", 64.18, -51.70, 2}, {"Scoresby Sund GL", 70.48, -21.97, 2},
    {"Nordostrundingen GL", 81.50, -11.20, 2}, {"Jan Mayen", 71.00, -8.50, 1}, {"Svalbard", 78.20, 15.60, 1},
    {"Azores", 37.74, -25.67, 1}, {"Flores Azores", 39.45, -31.20, 1}, {"Canaries", 28.10, -15.40, 1},
    {"Cape Verde", 14.90, -23.50, 1}, {"Bermuda", 32.30, -64.80, 2}, {"St Peter Paul Rocks", 0.92, -29.35, 2},
    {"Fernando de Noronha", -3.85, -32.42, 2}, {"Ascension", -7.95, -14.36, 1}, {"Tristan", -37.10, -12.30, 1},
    {"South Georgia", -54.28, -36.50, 2}, {"Falklands", -51.70, -57.85, 2}, {"New York", 40.70, -74.00, 2},
    {"Phoenix", 33.45, -112.07, 2}, {"Seattle", 47.60, -122.30, 2}, {"Anchorage", 61.20, -149.90, 2},
    {"Attu AK", 52.90, 172.90, 2}, {"Adak AK", 51.88, -176.65, 2}, {"Little Diomede", 65.75, -168.95, 2},
    {"Big Diomede RU", 65.78, -169.06, 1}, {"Cape Dezhnev RU", 66.08, -169.65, 1}, {"Anadyr RU", 64.73, 177.50, 1},
    {"Petropavlovsk RU", 53.00, 158.65, 1}, {"Commander Is RU", 55.20, 165.98, 1}, {"Honolulu", 21.30, -157.86, 2},
    {"Midway", 28.20, -177.37, 2}, {"Johnston", 16.73, -169.53, 2}, {"Wake", 19.28, 166.65, 3},
    {"Guam", 13.44, 144.79, 3}, {"Palmyra", 5.88, -162.08, 3}, {"Kingman Reef", 6.40, -162.40, 3},
    {"Jarvis", -0.37, -159.99, 3}, {"Baker", 0.19, -176.48, 3}, {"American Samoa", -14.27, -170.70, 3},
    {"Kiritimati", 1.87, -157.40, 3}, {"Tahiti", -17.53, -149.57, 3}, {"Pitcairn", -25.07, -130.10, 3},
    {"Easter Island", -27.12, -109.35, 2}, {"Galapagos", -0.74, -90.30, 2}, {"Clipperton", 10.30, -109.20, 2},
    {"Tokyo", 35.70, 139.70, 3}, {"Sapporo", 43.06, 141.35, 3}, {"Wakkanai", 45.40, 141.70, 3},
    {"Nemuro", 43.33, 145.58, 3}, {"Yuzhno-Sakhalinsk RU", 46.96, 142.74, 1}, {"Kunashir RU", 44.00, 145.80, 1},
    {"Vladivostok RU", 43.12, 131.89, 1}, {"Khabarovsk RU", 48.48, 135.08, 1},
    {"Blagoveshchensk RU", 50.26, 127.53, 1}, {"Heihe CN", 50.24, 127.49, 3}, {"Harbin CN", 45.75, 126.65, 3},
    {"Hailar CN", 49.20, 119.70, 3}, {"Manzhouli CN", 49.60, 117.40, 3}, {"Choibalsan MN", 48.07, 114.50, 1},
    {"Ulaanbaatar MN", 47.90, 106.90, 1}, {"Erenhot CN", 43.65, 111.97, 3}, {"Beijing", 39.90, 116.40, 3},
    {"Seoul", 37.57, 126.98, 3}, {"Pyongyang", 39.00, 125.75, 3}, {"Urumqi CN", 43.80, 87.60, 3},
    {"Kashgar CN", 39.47, 75.99, 3}, {"Almaty KZ", 43.24, 76.95, 1}, {"Bishkek KG", 42.87, 74.60, 1},
    {"Osh KG", 40.53, 72.80, 1}, {"Murghab TJ", 38.17, 73.97, 1}, {"Dushanbe TJ", 38.56, 68.79, 1},
    {"Termez UZ", 37.22, 67.28, 1}, {"Tashkent UZ", 41.30, 69.24, 1}, {"Ashgabat TM", 37.95, 58.38, 1},
    {"Serhetabat TM", 35.27, 62.35, 1}, {"Kabul AF", 34.53, 69.17, 3}, {"Mazar AF", 36.70, 67.10, 3},
    {"Herat AF", 34.35, 62.20, 3}, {"Islamabad PK", 33.70, 73.05, 3}, {"Karachi PK", 24.86, 67.00, 3},
    {"Gwadar PK", 25.12, 62.32, 3}, {"Delhi", 28.60, 77.20, 3}, {"Tehran IR", 35.70, 51.40, 3},
    {"Tabriz IR", 38.08, 46.29, 3}, {"Ardabil IR", 38.25, 48.29, 3}, {"Quchan IR", 37.10, 58.50, 3},
    {"Mashhad IR", 36.30, 59.60, 3}, {"Ahvaz IR", 31.32, 48.67, 3}, {"Bushehr IR", 28.97, 50.84, 3},
    {"Bandar Abbas IR", 27.18, 56.27, 3}, {"Chabahar IR", 25.29, 60.64, 3}, {"Abadan IR", 30.34, 48.30, 3},
    {"Basra IQ", 30.50, 47.80, 1}, {"Baghdad IQ", 33.30, 44.40, 1}, {"Erbil IQ", 36.19, 44.01, 1},
    {"Sulaymaniyah IQ", 35.56, 45.43, 1}, {"Kuwait", 29.37, 47.98, 1}, {"Doha", 25.29, 51.53, 1},
    {"Dubai", 25.20, 55.27, 1}, {"Musandam OM", 26.20, 56.25, 1}, {"Muscat OM", 23.59, 58.40, 1},
    {"Ras al Hadd OM", 22.53, 59.80, 1}, {"Riyadh", 24.70, 46.70, 1}, {"Van TR", 38.50, 43.40, 1},
    {"Hakkari TR", 37.57, 43.74, 1}, {"Erzurum TR", 39.90, 41.27, 1}, {"Dogubayazit TR", 39.55, 44.08, 1},
    {"Tbilisi GE", 41.70, 44.80, 1}, {"Yerevan AM", 40.18, 44.51, 1}, {"Meghri AM", 38.90, 46.24, 1},
    {"Baku AZ", 40.40, 49.87, 1}, {"Lankaran AZ", 38.75, 48.85, 1}, {"Nakhchivan AZ", 39.21, 45.41, 1},
    {"Moscow", 55.75, 37.60, 1}, {"Kazan", 55.80, 49.10, 1}, {"Novosibirsk", 55.00, 82.90, 1},
    {"Franz Josef Land", 80.60, 58.00, 1}, {"Mauritius", -20.16, 57.50, 1}, {"Reunion", -21.10, 55.50, 1},
    {"Seychelles", -4.62, 55.45, 1}, {"Kerguelen", -49.35, 70.20, 3}, {"Sydney", -33.87, 151.20, 3},
    {"Auckland", -36.85, 174.76, 3}, {"Chatham NZ", -43.95, -176.55, 3}, {"Tonga", -21.14, -175.20, 3},
    {"Fiji", -18.14, 178.44, 3}, {"Singapore", 1.35, 103.82, 3}, {"Maldives", 4.17, 73.51, 3},
    {"Sri Lanka", 6.93, 79.85, 3},
};

#define LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* The region at the centre of the place's locator of len characters (grid: 9 bytes) */
static int by_locator(int i, int len, char *grid) {
    double lat, lon;

    qth_pos_to_str(places[i].lat, places[i].lon, grid);
    grid[len] = 0;
    qth_str_to_pos(grid, &lat, &lon);
    return itu_region(lat, lon);
}

int main(void) {
    char grid[9];
    int  bad = 0, squares = 0;

    for (int i = 0; i < LEN(places); i++) {
        int got = itu_region(places[i].lat, places[i].lon);

        if (got != places[i].want) {
            printf("WRONG %-22s %7.2f %8.2f: Region %d, not %d\n", places[i].name, places[i].lat, places[i].lon, got,
                   places[i].want);
            bad++;
        }
        got = by_locator(i, 6, grid);
        if (got != places[i].want) {
            printf("WRONG %-22s %s: Region %d, not %d\n", places[i].name, grid, got, places[i].want);
            bad++;
        }
        got = by_locator(i, 4, grid);
        if (got != places[i].want) {
            printf("      %-22s %s:   Region %d, not %d (a border through the square)\n", places[i].name, grid, got,
                   places[i].want);
            squares++;
        }
    }
    /* Out of range: taken back into it (the North Pole, 60 W in Brazil, the South Pole south of Australia) */
    if (itu_region(91, 0) != 1 || itu_region(-3, 300) != 2 || itu_region(-95, 130) != 3) {
        printf("WRONG out of range\n");
        bad++;
    }
    printf("%d places: %d wrong at their coordinates or 6-character locators; %d 4-character squares across a "
           "border\n", LEN(places), bad, squares);
    return bad != 0;
}
