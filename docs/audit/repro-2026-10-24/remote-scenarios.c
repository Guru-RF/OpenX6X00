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

    snprintf(sessions[0].token, sizeof(sessions[0].token), "%032d", 7);
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

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    ctl_init();
    if (argc >= 2 && !strcmp(argv[1], "ping")) return run_ping();
    if (argc >= 2 && !strcmp(argv[1], "ptt")) return run_ptt();
    if (argc >= 2 && !strcmp(argv[1], "log")) return run_log();
    if (argc >= 2 && !strcmp(argv[1], "set")) return run_set();
    fprintf(stderr, "remote ptt|ping|log|set\n");
    return 2;
}
