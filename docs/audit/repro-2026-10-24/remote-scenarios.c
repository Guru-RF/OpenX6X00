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

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    ctl_init();
    if (argc >= 2 && !strcmp(argv[1], "ping")) return run_ping();
    if (argc >= 2 && !strcmp(argv[1], "ptt")) return run_ptt();
    fprintf(stderr, "remote ptt\n");
    return 2;
}
