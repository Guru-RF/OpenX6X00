/* Scenarios for the remote server's commands (make-remote-harness.py): ./remote ptt */

static client_t *open_client(int i, uint8_t role, bool tx) {
    client_t *c = &clients[i];
    memset(c, 0, sizeof(*c));
    pthread_mutex_init(&c->lock, NULL);
    c->fd = 10 + i, c->up = true, c->panel = true, c->sess = 0, c->role = role, c->tx_ok = tx;
    said[i][0] = 0;
    return c;
}

static void say(client_t *c, const char *text) {
    char b[512];
    snprintf(b, sizeof(b), "%s", text);
    printf("%7.3f  page %d -> %s\n", (fake_ms - 1000000) / 1000.0, (int)(c - clients), text);
    handle_text(c, b);
}

/* Review of C02: the pong carries the ping's number, so the page tells which ping it answers when the radio dropped
 * one (its queue for the page full); a ping without one, from a page before this, gets the bare pong */
static int run_ping(void) {
    client_t *c = open_client(0, ROLE_VIEW, false);

    say(c, "ping");
    say(c, "ping 7");
    say(c, "ping 9007199254740991");
    for (char *t = said[0]; *t; t++)
        if (*t == '\n') *t = '|';
    printf("pongs: %s\n", said[0]);
    return 0;
}

/* What the page sent, as the radio received it; the server loop checks the lease every 100 ms */
static uint64_t tick;
static void at(client_t *c, uint64_t ms, const char *text) {
    for (; tick <= 1000000 + ms; tick += 100) { fake_ms = tick; ptt_lease(c, fake_ms); }
    fake_ms = 1000000 + ms;
    if (text) say(c, text);
}

/* Audit C02: a hold on a link that stops delivering for 13 s. The radio lets go after 3 s without renewal; the
 * renewals the page sent meanwhile arrive together when the link delivers again, the operator's thumb still on
 * the button. Then a press on purpose. */
static int run_ptt(void) {
    client_t *c = open_client(0, ROLE_OPERATE, true);
    int downs;

    tick = fake_ms;
    at(c, 0, "key PTT 1 1");
    at(c, 1000, "key PTT 1 1");
    at(c, 2000, "key PTT 1 1");
    at(c, 15000, NULL);
    puts("        -- the link delivers again: what the page sent before it saw the stall, then its release");
    downs = key_downs;
    at(c, 15000, "key PTT 1 1");
    at(c, 15000, "key PTT 1 1");
    at(c, 15000, "key PTT 1 1");
    at(c, 15000, "key PTT 0");
    printf("late renewals of the ended hold keyed the radio: %d\n", key_downs - downs);
    downs = key_downs;
    at(c, 17000, "key PTT 1 2");
    at(c, 18000, "key PTT 1 2");
    printf("a new press keyed the radio: %d, still keyed at its renewal: %d\n", key_downs - downs, keyed);
    at(c, 18500, "key PTT 0");

    puts("        -- a press refused (control at the radio), renewed after control came free");
    ctl_claim(CTL_LOCAL, 0, "", true);
    downs = key_downs;
    at(c, 20000, "key PTT 1 3");
    ctl_release(CTL_LOCAL, 0);
    at(c, 21000, "key PTT 1 3");
    printf("the refused press's renewal keyed the radio: %d\n", key_downs - downs);

    puts("        -- a page from before the numbers (open since an update)");
    downs = key_downs;
    at(c, 23000, "key PTT 1");
    printf("a page without numbers keyed the radio: %d, told to reload: %d\n", key_downs - downs, strstr(said[0], "reload the page") != NULL);
    return 0;
}

/* Audit C03: two tabs of one paired browser share its session. Pages before the fix counted their QSOs from 1
 * in each tab; the fixed page picks 53 random bits. The GUI writes what it is given (gui_run) */
static int run_log(void) {
    client_t *a = open_client(0, ROLE_OPERATE, true), *b = open_client(1, ROLE_OPERATE, true);

    memset(sessions[0].hash, 7, sizeof(sessions[0].hash));
    snprintf(sessions[0].name, sizeof(sessions[0].name), "Shack PC");
    nsessions = 1;
    puts("        -- two tabs, each its QSO number 1, two different contacts");
    say(a, "logq 1 ON4AAA|59|59|Anna");
    gui_run(); log_flush();
    size_t before = strlen(said[1]);       /* tab B heard tab A's receipt: the session's */
    say(b, "logq 1 DL2BBB|57|55|Bernd");
    gui_run(); log_flush();
    say(b, "logq? 1 DL2BBB|57|55|Bernd");
    printf("one number, two contacts: writes=%d, the second told ok=%d, told conflict=%d\n", writes,
           strstr(said[1] + before, "logged 1 ok") != NULL, strstr(said[1] + before, "logged 1 conflict") != NULL);
    puts("        -- the fixed page's numbers: one each, a lost receipt sent again");
    say(a, "logq 6061235617123456 OK1CCC|59|59|Cyril");
    say(b, "logq 3101998844556677 SP3DDD|59|59|Dorota");
    gui_run(); log_flush();
    say(a, "logq 6061235617123456 OK1CCC|59|59|Cyril");
    gui_run(); log_flush();
    say(b, "logq? 3101998844556677 SP3DDD|59|59|Dorota");
    printf("two tabs, two more contacts: writes=%d, 53-bit numbers answered whole=%d\n", writes,
           strstr(said[1], "logged 3101998844556677 ok") != NULL && strstr(said[0], "logged 6061235617123456 ok") != NULL);
    return 0;
}

/* Audit C13: a "set" the radio refuses is answered in words and the whole state goes out to that page again
 * ("state_due", which the panel thread sends changed or not), so a page that showed what it asked for shows the
 * radio's; one the GUI thread would have dropped without a word (a lock, a frequency out of range, a full queue)
 * is refused the same way */
static void set_case(client_t *c, const char *what, const char *cmd) {
    said[c - clients][0] = 0;
    atomic_store(&c->state_due, false);
    say(c, cmd);
    int queued = njobs;
    njobs = 0;
    printf("%s queued=%d err=%d state again=%d\n", what, queued, !strncmp(said[c - clients], "err", 3),
           atomic_load(&c->state_due));
}

static int run_set(void) {
    client_t *viewer = open_client(0, ROLE_VIEW, false), *op = open_client(1, ROLE_OPERATE, true);

    viewer->sess = -1;
    snprintf(sessions[0].name, sizeof(sessions[0].name), "Kitchen phone");
    nsessions = 1;
    set_case(viewer, "view only:", "set freq 14075000");
    ctl_claim(CTL_LOCAL, 0, "", true);
    set_case(op, "control at the radio:", "set freq 14075000");
    ctl_claim(CTL_BROWSER, 1, "Kitchen phone", true);
    freq_locked = true;
    set_case(op, "frequency locked:", "set freq 14075000");
    freq_locked = false;
    set_case(op, "out of range:", "set freq 99000000");
    set_case(op, "a mode this radio has not:", "set mode 9");
    queue_full = true;
    set_case(op, "GUI queue full:", "set vol 20");
    queue_full = false;
    /* not "busy...": a page takes that for its PTT's refusal and stops renewing a press that may have keyed */
    printf("the queue-full refusal: %s", said[1]);
    set_case(op, "no such setting:", "set bogus 1");
    set_case(op, "accepted:", "set freq 14075000");
    return 0;
}

/* Issue #58: the pairing code had no limit. A burned code (three wrong tries) made room for the next at once:
 * three guesses a round, a round a second, the code found within the hour, and the first browser paired is the
 * admin. A browser now gets a new code at most every 30 s, and after each burned code the wait doubles, up to an
 * hour; a code asked on the radio itself (Settings, Pair code) comes at once and starts the waits over */
static void wrong_code(client_t *c) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "pair %06d Eve", (atoi(pair_code) + 1) % 1000000);
    say(c, cmd);
}

static int run_pair(void) {
    client_t *a = open_client(0, ROLE_VIEW, false);
    char      waits[128] = "", code[8];
    int       k = 0;

    a->sess = -1;
    say(a, "pair?");
    snprintf(code, sizeof(code), "%s", pair_code);
    fake_ms += 10000;
    say(a, "pair?");                                  /* the code still good: shown again, the same one */
    printf("a code: shown=%d, the same code asked again 10 s later=%d\n", codes_shown, !strcmp(code, pair_code));
    for (int round = 0; round < 8; round++) {
        uint64_t burned;
        say(a, "pair?");
        if (!pair_code[0]) {
            printf("no code for round %d: %s", round, said[0]);
            return 1;
        }
        wrong_code(a); wrong_code(a); wrong_code(a);
        gui_run();
        burned = fake_ms;
        said[0][0] = 0;
        say(a, "pair?");                              /* at once: paused */
        if (round == 0)
            printf("asked at once after a burned code: %s", said[0]);
        while (1) {                                   /* how long until a browser has a code again: asked every second */
            char ask[] = "pair?";
            fake_ms += 1000;
            said[0][0] = 0;
            quiet = 1;
            handle_text(a, ask);
            quiet = 0;
            if (!strncmp(said[0], "pairing code shown", 18))
                break;
        }
        k += snprintf(waits + k, sizeof(waits) - k, " %llu", (unsigned long long)((fake_ms - burned) / 1000));
    }
    printf("burned codes, then the wait for a new one in s:%s\n", waits);
    printf("the radio's screen said: %s\n", shown);
    wrong_code(a); wrong_code(a); wrong_code(a);     /* the 9th code burned: paused an hour */
    gui_run();
    said[0][0] = 0;
    say(a, "pair?");
    printf("a browser after the 9th: %s", said[0]);
    char at_radio[8];
    int  at_once = pair_show(at_radio, sizeof(at_radio), true) == 0 && at_radio[0];   /* Settings, Pair code */
    wrong_code(a); wrong_code(a); wrong_code(a);
    gui_run();
    fake_ms += 59000;
    said[0][0] = 0;
    say(a, "pair?");
    int one_min = !strncmp(said[0], "err pairing paused: a new code in", 33) && strstr(said[0], " s,") != NULL;
    printf("asked at the radio: a code at once=%d, burned again: paused 1 min, not an hour=%d\n", at_once, one_min);
    fake_ms += 2000;
    say(a, "pair?");
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "pair %s Kitchen phone", pair_code);
    int before = nsessions;
    say(a, cmd);
    int paired = nsessions == before + 1;
    say(a, "pair 123456 Eve");                        /* no code shown: refused, nothing counted */
    printf("the right code: paired=%d; a guess with no code shown counted=%d\n", paired, pair_tries != 0);
    return 0;
}

/* Issue #58: the tokens of the paired browsers were kept as they are in REMOTE_SESSIONS (/mnt), which the web
 * configurator's Files page hands out: a token copied from there was that browser's access, from the internet too
 * with Tailscale Funnel. The radio keeps their SHA-256 now. A file from before is read, its tokens hashed, and
 * written anew: the browsers paired before stay paired */
static void file_text(char *buf, size_t n) {
    FILE  *f = fopen(sessions_path, "r");
    size_t k = f ? fread(buf, 1, n - 1, f) : 0;
    buf[k] = 0;
    if (f) fclose(f);
}

static int run_tokens(void) {
    const char *t1 = "0123456789abcdef0123456789abcdef", *t2 = "fedcba9876543210fedcba9876543210";
    char        text[2048], cmd[128];
    client_t   *c = open_client(0, ROLE_VIEW, false);
    FILE       *f = fopen(sessions_path, "w");

    if (!f) return 2;
    fprintf(f, "%s\tShack PC\t2\t1\t1761000000\t1761500000\t192.168.1.20\n", t1);
    fprintf(f, "%s\tKitchen phone\t1\t1\t1761100000\t1761600000\t192.168.1.31\n", t2);
    fprintf(f, "zz23456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\tbroken\t1\t1\t0\t0\t-\n");
    fclose(f);
    sess_load();
    file_text(text, sizeof(text));
    printf("a file from before: %d sessions loaded, the tokens in the file now=%d\n", nsessions,
           strstr(text, t1) != NULL || strstr(text, t2) != NULL);
    printf("the file now:\n%s", text);
    c->sess = -1;
    snprintf(cmd, sizeof(cmd), "auth %s", t2);
    say(c, cmd);
    printf("the phone paired before the update: %s", strstr(said[0], "role operate 1 Kitchen phone") ? "operates" : said[0]);
    printf("\n");
    snprintf(cmd, sizeof(cmd), "auth %.31sf", t2);
    said[0][0] = 0;
    say(c, cmd);
    printf("its token with the last digit changed: %s", said[0]);
    nsessions = 0;
    sess_load();                                      /* read back as now written */
    c->sess = -1;
    snprintf(cmd, sizeof(cmd), "auth %s", t1);
    said[0][0] = 0;
    say(c, cmd);
    printf("read again: %d sessions, the PC paired before: %s\n", nsessions,
           strstr(said[0], "role admin 1 Shack PC") ? "admin" : said[0]);
    c->sess = -1;
    c->role = ROLE_VIEW;
    pair_code[0] = 0;
    pair_next_ms = 0;
    say(c, "pair?");
    snprintf(cmd, sizeof(cmd), "pair %s Laptop", pair_code);
    said[0][0] = 0;
    say(c, cmd);
    char token[TOKEN_LEN + 1] = "";
    sscanf(said[0], "paired %32s", token);
    file_text(text, sizeof(text));
    uint8_t h[32];
    char    hex[65];
    ws_sha256((const uint8_t *)token, strlen(token), h);
    for (int k = 0; k < 32; k++) snprintf(hex + k * 2, 3, "%02x", h[k]);
    printf("a new pairing: token handed out=%d, in the file=%d, its hash in the file=%d\n", (int)strlen(token) == TOKEN_LEN,
           token[0] && strstr(text, token) != NULL, strstr(text, hex) != NULL);
    return 0;
}

/* Issue #58: a browser at work over Tailscale Funnel (its token was good) may not pair from there: the code is
 * neither shown nor burned. A wrong token after a good one closes it and counts towards Funnel's pause, where a
 * LAN client only falls back to watching */
static int run_funnel(void) {
    client_t *f = open_client(0, ROLE_OPERATE, true), *l = open_client(1, ROLE_VIEW, false);
    char      cmd[64];

    f->funnel = true;
    l->sess   = -1;
    pair_code[0] = 0;
    pair_next_ms = 0;
    int shown_before = codes_shown;
    say(f, "pair?");
    snprintf(cmd, sizeof(cmd), "pair %06d Eve", 123456);
    say(f, cmd);
    printf("over Funnel: pair? and pair refused=%d, a code shown=%d, tries counted=%d\n",
           strstr(said[0], "err pairing refused over the internet") != NULL, codes_shown - shown_before, pair_tries);
    say(f, "auth 0123456789abcdef0123456789abcdee");
    int f_closed = f->fd < 0, counted = funnel_refused;
    say(l, "auth 0123456789abcdef0123456789abcdee");
    printf("over Funnel, a wrong key: closed=%d, counted=%d; on the LAN: closed=%d, watching=%d\n", f_closed, counted,
           l->fd < 0, l->role == ROLE_VIEW && l->fd >= 0);
    return 0;
}

/* Issue #58, "Use away from home": a paired browser at home asks for its own key for away; the radio makes a
 * session "NAME (away)" with its role and TX and answers with the token (to that browser alone). Asked again, the
 * one before is revoked: one away key per browser. Not from the internet, not unpaired, not from an away key */
static int run_away(void) {
    client_t *a = open_client(0, ROLE_ADMIN, true), *b = open_client(1, ROLE_OPERATE, true);
    char      first[TOKEN_LEN + 1] = "", second[TOKEN_LEN + 1] = "", cmd[64];

    nsessions = 0;
    a->sess = sess_new("Shack PC", "192.168.1.20", first);          /* the first: admin */
    b->sess = sess_new("Kitchen phone", "192.168.1.31", first);
    a->role = sessions[a->sess].role;
    b->role = sessions[b->sess].role;
    said[1][0] = 0;
    say(b, "away");
    sscanf(said[1], "away %32s", first);
    int i = sess_find(first);
    printf("away: answered with a key=%d, its session \"%s\" %s tx=%d\n", (int)strlen(first) == TOKEN_LEN,
           i >= 0 ? sessions[i].name : "?", i >= 0 ? role_name(sessions[i].role) : "?", i >= 0 && sessions[i].tx);
    int before = nsessions;
    said[1][0] = 0;
    say(b, "away");
    sscanf(said[1], "away %32s", second);
    printf("away again: a new key=%d, the one before gone=%d, sessions %d -> %d\n", strcmp(first, second) != 0 &&
           sess_find(second) >= 0, sess_find(first) < 0, before, nsessions);
    said[0][0] = 0;
    say(a, "away");
    sscanf(said[0], "away %32s", cmd);
    i = sess_find(cmd);
    printf("the admin's away key: %s\n", i >= 0 ? role_name(sessions[i].role) : said[0]);
    client_t *f = open_client(2, ROLE_OPERATE, true);
    f->funnel = true;
    f->sess = sess_find(second);
    say(f, "away");
    printf("from the internet: %s", said[2]);
    client_t *v = open_client(3, ROLE_VIEW, false);
    v->sess = -1;
    say(v, "away");
    printf("unpaired: %s", said[3]);
    v->sess = sess_find(second);
    v->role = ROLE_OPERATE;
    said[3][0] = 0;
    say(v, "away");
    printf("with its away key at home: %s", said[3]);
    return 0;
}

/* SHA-256 against the standard's examples and lengths at the padding's edges (55, 56, 64 bytes) */
static int run_sha(void) {
    static char mil[1000001];
    const char *in[] = {"", "abc", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                        "0123456789abcdef0123456789abcdef0123456789abcdef0123456", /* 55 */
                        "0123456789abcdef0123456789abcdef0123456789abcdef01234567", /* 56 */
                        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", /* 64 */
                        mil};
    memset(mil, 'a', 1000000);
    for (size_t i = 0; i < sizeof(in) / sizeof(in[0]); i++) {
        uint8_t h[32];
        ws_sha256((const uint8_t *)in[i], strlen(in[i]), h);
        printf("sha256(%zu bytes) = ", strlen(in[i]));
        for (int k = 0; k < 32; k++) printf("%02x", h[k]);
        printf("\n");
    }
    printf("same=%d, differs in the last byte=%d\n", ws_same("abcd", "abcd", 4), ws_same("abcd", "abce", 4));
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    ctl_init();
    snprintf(sessions_path, sizeof(sessions_path), "%s/remote_sessions.conf", getenv("AUDIT_TMP") ? getenv("AUDIT_TMP") : "/tmp");
    if (argc >= 2 && !strcmp(argv[1], "tokens")) return run_tokens();
    if (argc >= 2 && !strcmp(argv[1], "sha")) return run_sha();
    if (argc >= 2 && !strcmp(argv[1], "funnel")) return run_funnel();
    if (argc >= 2 && !strcmp(argv[1], "away")) return run_away();
    if (argc >= 2 && !strcmp(argv[1], "ping")) return run_ping();
    if (argc >= 2 && !strcmp(argv[1], "ptt")) return run_ptt();
    if (argc >= 2 && !strcmp(argv[1], "log")) return run_log();
    if (argc >= 2 && !strcmp(argv[1], "set")) return run_set();
    if (argc >= 2 && !strcmp(argv[1], "pair")) return run_pair();
    fprintf(stderr, "remote ptt|ping|log|set|pair|tokens|sha|funnel|away\n");
    return 2;
}
