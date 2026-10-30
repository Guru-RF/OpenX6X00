/* Issue #92: the mode of a QSO logged by hand (the browser Panel's Log tab, the CW keyboard, Field Ops' Log QSO),
 * with the production src/field/flog.c: field_qso_mode as dialog_field.c's adif_mode calls it, the record
 * field_adif_record writes, and the log read back as Field Ops shows it.
 *   qso-mode            the cases below; ends "N of N cases as they should be"
 *   qso-mode app NAME.. each app name (as the frequency strip has it, run.py takes them from screen_main.c) with
 *                       the mode it logs in USB-D away from FT8 and FT4: "NAME -> MODE" */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/field/flog.h"
#include "src/radio_types.h"

static int cases, good;

static void check(const char *what, int ok, const char *got) {
    cases++;
    good += ok != 0;
    printf("%s: %s%s\n", what, got, ok ? "" : "   <-- not as it should be");
}

static void mode_is(const char *app, int mode, int32_t hz, const char *want) {
    static const char *const names[] = {"LSB", "LSB-D", "USB", "USB-D", "CW", "CW-R", "AM", "NFM", "SAM", "WFM"};
    char                     what[96];
    const char              *got = field_qso_mode(app, mode, hz);

    snprintf(what, sizeof(what), "%s, %s, %.3f kHz", app ? app : "no app", names[mode], hz / 1000.0);
    check(what, !strcmp(got, want), got[0] ? got : "(no mode)");
}

/* The MODE and SUBMODE field_adif_record writes for a log mode */
static void record_is(const char *mode, const char *want_mode, const char *want_sub) {
    field_qso_t     q   = {.t = 1793000000, .call = "DL1ABC", .freq_hz = 14070000, .rst_s = "599", .rst_r = "579"};
    field_station_t sta = {.call = "ON6URE", .grid = "JO20"};
    char            rec[768], m[32] = "", s[32] = "", what[64];
    const char     *p;

    snprintf(q.mode, sizeof(q.mode), "%s", mode);
    field_adif_record(rec, sizeof(rec), &q, &sta);
    if ((p = strstr(rec, "<MODE:")))
        sscanf(strchr(p, '>') + 1, "%31s", m);
    if ((p = strstr(rec, "<SUBMODE:")))
        sscanf(strchr(p, '>') + 1, "%31s", s);
    char got[80];
    snprintf(got, sizeof(got), "MODE %s SUBMODE %s", m[0] ? m : "-", s[0] ? s : "-");
    snprintf(what, sizeof(what), "record of %s", mode[0] ? mode : "a QSO with no mode");
    check(what, !strcmp(m, want_mode) && !strcmp(s, want_sub), got);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "app")) {
        for (int i = 2; i < argc; i++) {
            const char *m = field_qso_mode(argv[i], radio_mode_usb_dig, 14070000);
            printf("%s -> %s\n", argv[i], m[0] ? m : "(no mode)");
        }
        return 0;
    }

    /* The app open names the mode, also on an FT8 or FT4 frequency */
    mode_is("PSK31", radio_mode_usb_dig, 14070000, "PSK31");
    mode_is("PSK63", radio_mode_usb_dig, 14070000, "PSK63");
    mode_is("PSK31", radio_mode_usb_dig, 14074500, "PSK31");
    mode_is("Olivia", radio_mode_usb_dig, 14071000, "OLIVIA");
    mode_is("JTTY", radio_mode_usb_dig, 14090000, "JTTY");
    mode_is("RTTY", radio_mode_lsb_dig, 14080000, "RTTY");
    mode_is("FT4", radio_mode_usb_dig, 14074000, "FT4");
    mode_is("FT2", radio_mode_usb_dig, 14084000, "FT2");
    mode_is("JS8", radio_mode_usb_dig, 14078000, "JS8");
    mode_is("SSTV", radio_mode_usb, 14230000, "SSTV");
    /* No app: the frequency in a data mode, FT8 or FT4 on theirs, no mode elsewhere (it was FT8) */
    mode_is(NULL, radio_mode_usb_dig, 14074000, "FT8");
    mode_is(NULL, radio_mode_usb_dig, 14080000, "FT4");
    mode_is(NULL, radio_mode_usb_dig, 14070000, "");
    mode_is(NULL, radio_mode_usb_dig, 14090000, "");
    mode_is(NULL, radio_mode_lsb_dig, 3580000, "");
    /* The receive-only apps log as no app does */
    mode_is("WeFax", radio_mode_usb_dig, 7880000, "");
    mode_is("NavTex", radio_mode_usb, 518000, "SSB");
    /* The voice and CW modes as before */
    mode_is(NULL, radio_mode_usb, 14200000, "SSB");
    mode_is(NULL, radio_mode_usb, 14074500, "FT8");
    mode_is(NULL, radio_mode_cw, 14030000, "CW");
    mode_is(NULL, radio_mode_cwr, 14074000, "CW");
    mode_is(NULL, radio_mode_am, 7200000, "AM");
    mode_is(NULL, radio_mode_nfm, 29600000, "FM");

    /* ADIF 3.1.8's MODE and SUBMODE */
    record_is("PSK31", "PSK", "PSK31");
    record_is("PSK63", "PSK", "PSK63");
    record_is("OLIVIA", "OLIVIA", "");
    record_is("JTTY", "MFSK", "JTTY");
    record_is("RTTY", "RTTY", "");
    record_is("FT8", "FT8", "");
    record_is("FT4", "MFSK", "FT4");
    record_is("JS8", "MFSK", "JS8");
    record_is("", "", "");

    /* Into a log and read back as Field Ops lists it */
    const char *tmp = getenv("AUDIT_TMP") ? getenv("AUDIT_TMP") : "/tmp";
    char        path[512], head[256] = "";
    const char *modes[] = {"PSK31", "PSK63", "OLIVIA", "JTTY", "RTTY", "FT4", ""};
    size_t      n_modes = sizeof(modes) / sizeof(modes[0]);
    snprintf(path, sizeof(path), "%s/qso-mode-field/logs/day.adi", tmp);
    for (size_t i = 0; i < n_modes; i++) {
        field_qso_t     q   = {.t = 1793000000 + 60 * (time_t)i, .freq_hz = 14070000, .rst_s = "599", .rst_r = "599"};
        field_station_t sta = {.call = "ON6URE"};
        snprintf(q.call, sizeof(q.call), "DL%zuABC", i);
        snprintf(q.mode, sizeof(q.mode), "%s", modes[i]);
        if (!field_log_add(path, &q, &sta)) {
            perror(path);
            return 1;
        }
    }
    FILE *f = fopen(path, "r");
    if (f && fgets(head, sizeof(head), f) && fgets(head, sizeof(head), f))
        head[strcspn(head, "\n")] = 0;
    if (f)
        fclose(f);
    check("the log's header", strstr(head, "<ADIF_VER:5>3.1.8 ") != NULL, head);
    field_qso_t back[8];
    int         n = field_log_read(path, back, 8), same = n == (int)n_modes;
    char        got[128] = "";
    for (int i = 0; i < n && i < 8; i++) {
        same = same && !strcmp(back[i].mode, modes[i]);
        snprintf(got + strlen(got), sizeof(got) - strlen(got), "%s%s", i ? " " : "", back[i].mode[0] ? back[i].mode : "-");
    }
    check("read back", same, got);

    printf("%d of %d cases as they should be\n", good, cases);
    return good == cases ? 0 : 1;
}
