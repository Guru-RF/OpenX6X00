/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of CB callsigns in the FT8/FT4 app (#286): the messages as WSJT-CB, the 27 MHz edition of the desktop
 *  program, puts them on the air between two CB calls, so that it and the radio decode each other.
 *
 *  1. The bits. Each message of such a QSO, as WSJT-CB makes it ("CQ 26AT101", "<1AT106> 26AT101", "<1AT106> 26AT101
 *     RR73", "26AT101 -12", "26AT101 R-12", "26AT101 RR73", "26AT101 73"), and the calls of WSJT-CB's own tests in
 *     them, through the app's encoder (msg_encode.c's ftx_encode_text, what the app sends) and through a packer
 *     written here from the 77-bit format: type 4, a 12-bit hash of one call, the other in 58 bits of base 38
 *     right-aligned, iflip, RRR/RR73/73, CQ, i3 = 4; free text, 13 characters of base 42 right-aligned, n3 = i3 = 0.
 *     The same 77 bits. Amateur messages go as before (type 1).
 *  2. The text. Each packed message decoded by ft8_lib as the radio decodes, with the hash table of a station that
 *     has its own call in it (worker.c): the text again, "<...>" for a hash it has not heard; the type 4 form with the
 *     hashed call second ("1AT106 <26AT101>"), which WSJT-CB also sends.
 *  3. On the air: two radios with the app's QSO sequence (qso.cpp), Auto on, 26AT101 calls CQ and 1AT106 answers it;
 *     each message made into audio by the app's encoder (worker.c), over noise, decoded by the other's decoder
 *     (worker.c and ft8_lib, a hash table of its own: its call and what it decodes) and handed to its sequence as
 *     ft8_window.c does. The messages are WSJT-CB's, and both log the QSO once with the reports the other sent. FT8
 *     and FT4.
 *  4. The country: none for a CB call (src/dxcc.c with the repository's cty.dat), where cty.dat would take the first
 *     characters for an amateur prefix (1A, the Order of Malta; 26 none, 21 none); amateur calls as before.
 */

#include "cb.h"
#include "dxcc.h"
#include "ft8/ft2.h"
#include "ft8/msg_encode.h"
#include "ft8/qso.h"
#include "ft8/worker.h"

#include <ft8lib/hashtable.h>
#include <ft8lib/message.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 11025                  /* the app's: 44.1 kHz / 4 */

static int  bad, cases;
static bool verbose;

static void check(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void check(bool ok, const char *fmt, ...) {
    va_list ap;

    cases++;
    if (ok && !verbose) return;
    bad += !ok;
    printf("%s ", ok ? "  ok " : "WRONG");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

/* ---- 1. the bits ---- */

/* The hash table emptied: hashtable_init() zeroes only half of what it takes from malloc (the size of a pointer for
 * that of an entry), so calls of a table freed before can be in it again. Twice: once old, then gone */
static void empty_table(void) {
    hashtable_cleanup(0);
    hashtable_cleanup(0);
}

static const char A38[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ/";
static const char A42[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+-./?";

typedef struct {
    uint8_t b[FTX_PAYLOAD_LENGTH_BYTES];
    int     n;
} bits_t;

/* nbits of v, the highest first, after those already there */
static void put(bits_t *w, uint64_t v, int nbits) {
    for (int i = nbits - 1; i >= 0; i--, w->n++)
        if ((v >> i) & 1) w->b[w->n / 8] |= 0x80 >> (w->n % 8);
}

static int place(const char *alphabet, char c) {
    const char *p = c ? strchr(alphabet, c) : NULL;
    return p ? (int)(p - alphabet) : -1;
}

/* A call's 11 characters in base 38: right-aligned (type 4's call in full), left-aligned (for its hash) */
static uint64_t base38(const char *call, bool right) {
    char     c11[12];
    uint64_t n = 0;

    snprintf(c11, sizeof(c11), right ? "%11s" : "%-11s", call);
    for (int i = 0; i < 11; i++) n = n * 38 + place(A38, c11[i]);
    return n;
}

/* The 12 bits of a call's hash: the top of 47055833459 times its left-aligned base 38, in 64 bits */
static uint32_t hash12(const char *call) {
    return (uint32_t)((47055833459ULL * base38(call, false)) >> 52);
}

/* Type 4: "<hashed> plain [RRR|RR73|73]" (iflip 0), "plain <hashed> ..." (iflip 1), "CQ plain" (icq, its own hash) */
static void ref_type4(uint8_t out[FTX_PAYLOAD_LENGTH_BYTES], const char *hashed, const char *plain, int iflip,
                      int nrpt, int icq) {
    bits_t w = {0};

    put(&w, hash12(icq ? plain : hashed), 12);
    put(&w, base38(plain, true), 58);
    put(&w, iflip, 1);
    put(&w, nrpt, 2);
    put(&w, icq, 1);
    put(&w, 4, 3);
    memcpy(out, w.b, sizeof(w.b));
}

/* Free text: 13 characters right-aligned in base 42 (71 bits), n3 0, i3 0 */
static void ref_free(uint8_t out[FTX_PAYLOAD_LENGTH_BYTES], const char *text) {
    char              c13[14];
    unsigned __int128 n = 0;
    bits_t            w = {0};

    snprintf(c13, sizeof(c13), "%13s", text);
    for (int i = 0; i < 13; i++) n = n * 42 + place(A42, c13[i]);
    put(&w, (uint64_t)(n >> 64), 7);
    put(&w, (uint64_t)n, 64);
    put(&w, 0, 3);
    put(&w, 0, 3);
    memcpy(out, w.b, sizeof(w.b));
}

static void hex(char *out, const uint8_t *b) {
    for (int i = 0; i < FTX_PAYLOAD_LENGTH_BYTES; i++) sprintf(out + 2 * i, "%02x", b[i]);
}

/* text through the app's encoder: the bits want */
static void same_bits(const char *text, const uint8_t *want, const char *form) {
    ftx_message_t msg;
    char          got_hex[24] = "", want_hex[24];
    bool          ok = ftx_encode_text(&msg, &hash_if, text) == FTX_MESSAGE_RC_OK;

    if (ok) hex(got_hex, msg.payload);
    hex(want_hex, want);
    check(ok && !memcmp(msg.payload, want, FTX_PAYLOAD_LENGTH_BYTES), "\"%s\" as %s (%s, %s wanted)", text, form,
          got_hex, want_hex);
}

static void bits(void) {
    static const char *const calls[] = {"1AT106", "26AT101", "161XZ085", "001AB123", "1AT1000", "999ZZ/ZZ",
                                        "999ZZ999", "21AT106"};
    static const char *const ends[]  = {"", "RRR", "RR73", "73"};
    uint8_t                  want[FTX_PAYLOAD_LENGTH_BYTES];
    char                     text[40];

    printf("1. the bits: the app's encoder and the 77-bit format\n");
    hashtable_init(256);
    empty_table();
    for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
        const char *c = calls[i];

        check(cb_callsign(c) && !ftx_call_is_standard(c), "%s: a CB call, not a standard one", c);
        snprintf(text, sizeof(text), "CQ %s", c);
        ref_type4(want, NULL, c, 0, 0, 1);
        same_bits(text, want, "type 4, CQ");
        for (int k = 0; k < 4; k++) {
            snprintf(text, sizeof(text), "<1AT106> %s%s%s", c, k ? " " : "", ends[k]);
            ref_type4(want, "1AT106", c, 0, k, 0);
            same_bits(text, want, "type 4");
        }
        static const char *const words[] = {"-12", "+05", "R-12", "R+05", "RRR", "RR73", "73"};
        for (size_t k = 0; k < sizeof(words) / sizeof(words[0]); k++) {
            snprintf(text, sizeof(text), "%s %s", c, words[k]);
            ref_free(want, text);
            same_bits(text, want, "free text, right-aligned");
        }
    }
    ref_free(want, "TNX 73 GL");
    same_bits("TNX 73 GL", want, "free text, right-aligned as WSJT-X packs it");

    /* Amateur calls: as before */
    static const char *const amateur[] = {"CQ ON6URE JO21", "A1AAA Q1QQQ -05", "A1AAA Q1QQQ RR73", "A1AAA Q1QQQ 73",
                                          "A1AAA Q1QQQ R-05"};
    for (size_t i = 0; i < sizeof(amateur) / sizeof(amateur[0]); i++) {
        ftx_message_t msg;
        check(ftx_encode_text(&msg, &hash_if, amateur[i]) == FTX_MESSAGE_RC_OK &&
                  ftx_message_get_type(&msg) == FTX_MESSAGE_TYPE_STANDARD,
              "\"%s\": type 1 still", amateur[i]);
    }
    /* 1A1: a CB call FT8 packs as a standard one, as WSJT-CB does: type 1, no CB forms */
    ftx_message_t msg;
    check(cb_callsign("1A1") && ftx_call_is_standard("1A1") &&
              ftx_encode_text(&msg, &hash_if, "CQ 1A1") == FTX_MESSAGE_RC_OK &&
              ftx_message_get_type(&msg) == FTX_MESSAGE_TYPE_STANDARD,
          "1A1: a CB call FT8 takes as a standard one, \"CQ 1A1\" type 1");
}

/* ---- 2. the text ---- */

/* A station's table: its own call in it, as worker.c keeps it, nothing else */
static void station(const char *own) {
    ftx_message_t msg;
    char          text[20];

    empty_table();
    snprintf(text, sizeof(text), "CQ %s", own);
    ftx_message_init(&msg);
    ftx_message_encode(&msg, &hash_if, text);
}

static void decodes_as(const uint8_t *payload, const char *want) {
    ftx_message_t msg;
    char          text[FTX_MAX_MESSAGE_LENGTH] = "";

    ftx_message_init(&msg);
    memcpy(msg.payload, payload, FTX_PAYLOAD_LENGTH_BYTES);
    check(ftx_message_decode(&msg, &hash_if, text) == FTX_MESSAGE_RC_OK && !strcmp(text, want), "decoded \"%s\" (%s)",
          want, text);
}

static void texts(void) {
    uint8_t p[FTX_PAYLOAD_LENGTH_BYTES];

    printf("2. the text: ft8_lib's decoder with a station's hash table\n");
    station("1AT106");
    ref_type4(p, NULL, "26AT101", 0, 0, 1);
    decodes_as(p, "CQ 26AT101");
    ref_type4(p, "1AT106", "26AT101", 0, 0, 0);
    decodes_as(p, "<1AT106> 26AT101");
    ref_type4(p, "1AT106", "26AT101", 0, 2, 0);
    decodes_as(p, "<1AT106> 26AT101 RR73");
    ref_type4(p, "26AT101", "1AT106", 1, 0, 0);
    decodes_as(p, "1AT106 <26AT101>");          /* 26AT101 heard in full in its CQ */
    ref_free(p, "1AT106 R-12");
    decodes_as(p, "1AT106 R-12");
    ref_free(p, "999ZZ999 R+05");
    decodes_as(p, "999ZZ999 R+05");
    ref_free(p, "1AT106 73");
    decodes_as(p, "1AT106 73");

    station("1AT106");
    ref_type4(p, "161XZ085", "999ZZ/ZZ", 0, 3, 0);
    decodes_as(p, "<...> 999ZZ/ZZ 73");         /* a hash not heard */
    hashtable_delete();
}

/* ---- 3. on the air ---- */

typedef struct {
    const char      *call;
    float            audio_hz;
    FTxQsoProcessor *qso;
    ftx_tx_msg_t     tx;
    int              logs, rcvd, sent;
    char             logged_call[16];
} radio_t;

static radio_t *saving;             /* the radio whose sequence is being fed */

static void on_saved(const char *call, const char *grid, const int rcvd, const int sent, bool aside) {
    (void)grid, (void)aside;
    saving->logs++;
    saving->rcvd = rcvd;
    saving->sent = sent;
    snprintf(saving->logged_call, sizeof(saving->logged_call), "%s", call);
}

typedef struct {
    int  n;
    char text[4][FTX_MAX_MESSAGE_LENGTH];
    int  snr[4];
    float hz[4];
} heard_t;

static void heard_cb(const char *text, int snr, float freq_hz, float time_sec, void *user) {
    heard_t *h = user;

    (void)time_sec;
    if (h->n < 4) {
        snprintf(h->text[h->n], sizeof(h->text[0]), "%s", text);
        h->snr[h->n] = snr;
        h->hz[h->n]  = freq_hz;
        h->n++;
    }
}

static float noise(void) {
    return 0.02f * ((float)rand() / (float)RAND_MAX - 0.5f);
}

/* text from `from` at its audio frequency, heard by `to`'s decoder: what it decoded */
static heard_t over_the_air(int proto, const radio_t *from, const radio_t *to, const char *text) {
    heard_t  h = {0};
    int16_t *tx;
    uint32_t ntx;

    ftx_worker_set_own_call(from->call);
    ftx_worker_init(RATE, proto);
    empty_table();
    bool made = ftx_worker_generate_tx_samples(text, (uint16_t)from->audio_hz, RATE, &tx, &ntx);
    ftx_worker_free();
    if (!made) return h;

    int    n   = (int)(ftx_slot_time(proto) * RATE);
    int    s0  = RATE / 2;
    float *buf = malloc(n * sizeof(float));

    for (int k = 0; k < n; k++) buf[k] = noise();
    for (uint32_t k = 0; k < ntx && s0 + (int)k < n; k++) buf[s0 + k] += tx[k] / 32768.0f * 0.02f;
    free(tx);

    ftx_worker_set_own_call(to->call);
    ftx_worker_init(RATE, proto);
    empty_table();                  /* of the sender's calls, from the worker before (empty_table) */
    ftx_worker_reset();             /* our call in it again (worker.c remember_own_call) */
    int bs = ftx_worker_get_block_size();
    for (int p = 0; p + bs <= n && !ftx_worker_is_full(); p += bs) {
        ftx_worker_put_rx_samples(buf + p, bs);
        ftx_worker_decode(heard_cb, false, &h);
    }
    ftx_worker_decode(heard_cb, true, &h);
    ftx_worker_free();
    free(buf);
    return h;
}

/* The app's sequence of `call`, as ft8_window.c makes it: Auto on, the call's kind from the encoder */
static void radio_on(radio_t *r, const char *call, float audio_hz) {
    memset(r, 0, sizeof(*r));
    r->call     = call;
    r->audio_hz = audio_hz;
    r->qso      = ftx_qso_processor_init(call, "JO21CV", on_saved, 6);
    ftx_qso_processor_set_auto(r->qso, true);
    ftx_qso_processor_set_local_standard(r->qso, ftx_call_is_standard(call));
}

static void on_the_air(int proto) {
    radio_t     a, b, *radio[2] = {&a, &b};
    char        said[8][FTX_MAX_MESSAGE_LENGTH];
    int         n_said = 0, a_report = 99, b_report = 99, quiet = 0;
    const char *name = ftx_protocol_name(proto);

    printf("3. on the air, %s: 26AT101 calls CQ, 1AT106 answers\n", name);
    radio_on(&a, "26AT101", 1500.0f);
    radio_on(&b, "1AT106", 1210.0f);
    snprintf(a.tx.msg, sizeof(a.tx.msg), "CQ %s", a.call);  /* cq_make_message: no grid with this call */
    a.tx.repeats = 6;

    for (int slot = 0; slot < 16 && quiet < 2; slot++) {
        radio_t *from = radio[slot % 2], *to = radio[1 - slot % 2];

        if (!from->tx.msg[0]) {
            quiet++;
        } else {
            char text[FTX_MAX_MESSAGE_LENGTH];

            quiet = 0;
            snprintf(text, sizeof(text), "%s", from->tx.msg);
            ftx_qso_processor_sent(from->qso, text);   /* transmit() */
            if (--from->tx.repeats <= 0) from->tx.msg[0] = '\0';
            if (n_said < 8) snprintf(said[n_said++], sizeof(said[0]), "%s", text);

            heard_t h = over_the_air(proto, from, to, text);
            check(h.n == 1 && !strcmp(h.text[0], text), "%s: \"%s\" from %s decoded by %s (%d: %s, %d dB, %.0f Hz)",
                  name, text, from->call, to->call, h.n, h.n ? h.text[0] : "", h.n ? h.snr[0] : 0,
                  h.n ? h.hz[0] : 0.0f);
            for (int i = 0; i < h.n; i++) {
                ftx_msg_meta_t meta = {.freq_hz = h.hz[i]};

                saving = to;
                ftx_qso_processor_add_rx_text(to->qso, h.text[i], h.snr[i], &meta, &to->tx);
                if (slot == 0 && meta.type == FTX_MSG_TYPE_CQ) {
                    ftx_qso_processor_start_qso(to->qso, &meta, &to->tx);     /* the CQ pressed (list_pressed) */
                }
                if (slot == 1 && to == &a) a_report = h.snr[i];     /* its call: A's report */
                if (slot == 2 && to == &b) b_report = h.snr[i];     /* A's report: B's R-report */
            }
        }
        saving = &a;
        ftx_qso_processor_start_new_slot(a.qso);
        saving = &b;
        ftx_qso_processor_start_new_slot(b.qso);
    }

    char want[6][FTX_MAX_MESSAGE_LENGTH];
    snprintf(want[0], sizeof(want[0]), "CQ 26AT101");
    snprintf(want[1], sizeof(want[1]), "<26AT101> 1AT106");
    snprintf(want[2], sizeof(want[2]), "1AT106 %+03d", a_report);
    snprintf(want[3], sizeof(want[3]), "26AT101 R%+03d", b_report);
    snprintf(want[4], sizeof(want[4]), "1AT106 RR73");
    snprintf(want[5], sizeof(want[5]), "26AT101 73");
    bool same = n_said == 6;
    for (int i = 0; same && i < 6; i++) same = !strcmp(said[i], want[i]);
    check(same, "%s: the messages WSJT-CB sends, in its order (%d sent)", name, n_said);
    if (!same || verbose)
        for (int i = 0; i < n_said; i++) printf("      %s\n", said[i]);
    check(a.logs == 1 && !strcmp(a.logged_call, "1AT106") && a.sent == a_report && a.rcvd == b_report,
          "%s: 26AT101 logged 1AT106 once: sent %d, rcvd %d (%d logs)", name, a.sent, a.rcvd, a.logs);
    check(b.logs == 1 && !strcmp(b.logged_call, "26AT101") && b.sent == b_report && b.rcvd == a_report,
          "%s: 1AT106 logged 26AT101 once: sent %d, rcvd %d (%d logs)", name, b.sent, b.rcvd, b.logs);
    ftx_qso_processor_delete(a.qso);
    ftx_qso_processor_delete(b.qso);
}

/* ---- 4. the country ---- */

static void countries(void) {
    static const char *const cb[] = {"26AT101", "1AT106", "21AT106", "1A1", "1A12", "999ZZ/ZZ", "<1AT106>", "161XZ085"};
    const char              *name, *prefix;

    printf("4. the country\n");
    for (size_t i = 0; i < sizeof(cb) / sizeof(cb[0]); i++)
        check(!dxcc_lookup(cb[i], &name, &prefix) && !dxcc_entity(cb[i]), "%s: no country", cb[i]);
    check(dxcc_lookup("1A0KM", &name, &prefix) && !strcmp(prefix, "1A"), "1A0KM: still %s",
          dxcc_lookup("1A0KM", &name, &prefix) ? name : "none");
    check(dxcc_lookup("ON6URE", &name, &prefix) && !strcmp(prefix, "ON"), "ON6URE: still Belgium");
    check(dxcc_lookup("2E0ABC", &name, &prefix) && !strcmp(prefix, "G"), "2E0ABC: still England");
}

int main(int argc, char **argv) {
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    setvbuf(stdout, NULL, _IOLBF, 0);
    srand(286);
    bits();
    texts();
    on_the_air(FTX_PROTOCOL_FT8);
    on_the_air(FTX_PROTOCOL_FT4);
    countries();
    printf("FT8 CB calls %s: %d of %d cases\n", bad ? "FAILED" : "OK", cases - bad, cases);
    return bad != 0;
}
