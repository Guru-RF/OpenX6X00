// Audit C02 (26 October), the page's side: the production RadioLink and RadioPtt with a fake socket and clock.
// A held PTT on a link that stops delivering: the renewals stop and the release goes out when the link is seen
// stalled, a press is refused until a ping sent after the silence is answered, and every press has its own
// number, so the radio can tell a late renewal from a press (the radio's side: remote-scenarios.c, "ptt").
// A pong the radio drops (its queue for the page full) leaves the link stalled only until a later ping is answered:
// pings are numbered, "ping N" answered "pong N", and an unanswered sync ping is sent again (review of C02).
// An err while a press waits for "tx 1" that is not the PTT's leaves the press; the PTT's own refusal ends it and
// lets go, the radio's microphone or the browser's (review of C13).
const fs = require('fs');
const vm = require('vm');
let now = 0, next = 1;
const timers = new Map();
function add(fn, ms, repeat) { const id = next++; timers.set(id, {fn, at: now + ms, ms, repeat}); return id; }
function advance(by) {
  const to = now + by;
  for (;;) {
    const entry = [...timers].filter(([, t]) => t.at <= to).sort((a, b) => a[1].at - b[1].at || a[0] - b[0])[0];
    if (!entry) break;
    const [id, t] = entry; now = t.at;
    if (t.repeat) t.at += t.ms; else timers.delete(id);
    t.fn();
  }
  now = to;
}
const sent = [];
let socket = null, paused = false, lose = 0, bare = false;
function FakeSocket() { socket = this; this.readyState = 0; add(() => { this.readyState = 1; this.onopen(); }, 10, false); }
FakeSocket.prototype.send = function(text) {
  sent.push({at: now, text});
  const ping = /^ping(?: (\d+))?$/.exec(text);      /* as remote_server.c: "ping N" -> "pong N", "ping" -> "pong" */
  if (!ping || paused) return;
  if (lose) { lose--; return; }                                          /* the radio's queue for the page full: no pong */
  add(() => this.onmessage({data: bare || !ping[1] ? 'pong' : 'pong ' + ping[1]}), 20, false);   /* the radio answers at once */
};
const pings = () => sent.filter(x => /^ping( \d+)?$/.test(x.text)).map(x => +x.text.slice(5));
FakeSocket.prototype.close = function() {};
const context = {window: {}, Date: {now: () => now}, WebSocket: FakeSocket, Promise,
  setTimeout: (fn, ms) => add(fn, ms, false), setInterval: (fn, ms) => add(fn, ms, true), clearTimeout: id => timers.delete(id), clearInterval: id => timers.delete(id)};
for (const f of ['radio_link.js', 'radio_session.js'])
  vm.runInNewContext(fs.readFileSync('webserver/src/x6100_webserver/static/js/' + f, 'utf8'), context);
let ptt = null;
const link = context.window.RadioLink.create({url: () => 'ws://radio/', ping: 'ping',
  onStall: on => { if (on) ptt.lost('The radio does not answer'); }});            /* as the Panel and Screen pages do */
ptt = context.window.RadioPtt.create({el: null, send: text => link.send(text), session: {can: () => ''}, mic: {wanted: () => false}, link});
const after = t => sent.filter(x => x.at > t).map(x => x.text);
const failures = [];
function check(ok, what) { console.log((ok ? 'ok    ' : 'FAIL  ') + what); if (!ok) failures.push(what); }

(async () => {
link.connect();
await new Promise(r => setImmediate(r));             /* the URL comes as a promise */
advance(100);
ptt.press();
advance(50);
ptt.radioTx(true);
advance(2000);
check(sent.filter(x => x.text === 'key PTT 1 1').length === 3, 'the press and its renewals carry the press\'s number: key PTT 1 1');
const pauseAt = now;
paused = true;                                       /* nothing comes back: no pong, no state */
advance(9000);
check(link.stalled, 'the link is seen stalled (a ping without an answer for 3 s)');
const stalledAt = sent.find(x => x.text === 'key PTT 0' && x.at > pauseAt);
check(!!stalledAt && ptt.state === 'ended', 'the hold is let go at the stall: key PTT 0 sent, the button says the transmission ended');
check(stalledAt && !after(stalledAt.at).some(t => /^key PTT 1/.test(t)), 'no renewal after the stall');
ptt.press();
check(ptt.state === 'refused' && !after(now - 1).some(t => /^key PTT 1/.test(t)), 'a press while the radio does not answer is refused, nothing sent');
paused = false;
const before = pings();
check(before.every((n, i) => n === i + 1), 'the pings are numbered from 1: ping 1, ping 2, ...');
socket.onmessage({data: 'state {}'});                /* the backlog arrives: not yet proof that the radio hears us */
check(link.stalled && pings().length === before.length + 1, 'the first message after the silence sends a ping; still stalled');
ptt.press();
check(!after(now - 1).some(t => /^key PTT 1/.test(t)), 'a press while the backlog drains is refused');
socket.onmessage({data: 'pong ' + before[before.length - 1]});   /* the answer to the ping from before the silence */
check(link.stalled, 'the pong of the ping from before the silence does not count');
advance(100);                                        /* the answer to the new one */
check(!link.stalled, 'the answer to the ping sent after the silence ends the stall');
advance(6000);                                       /* the refusal shown, then idle */
ptt.press();
advance(10);
check(after(now - 11).includes('key PTT 1 2'), 'a press on purpose afterwards has a new number: key PTT 1 2');
ptt.release();
advance(3000);

/* The radio drops a frame, a pong too, when its queue for the page is full (a picture and a busy cluster during a
 * long stall): a lost pong must not leave the link stalled for good, meters flowing and every press refused */
const untilStalled = () => { for (let i = 0; i < 15 && !link.stalled; i++) advance(1000); return link.stalled; };
lose = 1;
check(untilStalled(), 'a pong the radio dropped: the link is seen stalled');
socket.onmessage({data: 'm -95'});                   /* a meter: the radio is there */
advance(100);
check(!link.stalled, 'a pong lost: back as soon as a ping sent after it is answered');
lose = 2;                                            /* the next pong, and the sync ping's own */
untilStalled();
socket.onmessage({data: 'm -95'});
advance(100);
check(link.stalled, 'the sync ping\'s pong lost too: still stalled');
const n0 = pings().length;
advance(4000);
check(pings().length > n0 && !link.stalled, 'an unanswered sync ping is sent again; its answer ends the stall');
ptt.press();
advance(10);
check(after(now - 11).includes('key PTT 1 3'), 'a press afterwards keys: key PTT 1 3');
ptt.release();
advance(3000);

/* A radio from before the numbers answers "pong": the answer to the last ping */
bare = true;
advance(20000);
check(!link.stalled, 'a radio answering a bare pong: the link stays up');
lose = 1;
const bareStall = untilStalled();
socket.onmessage({data: 'm -95'});
advance(100);
check(bareStall && !link.stalled, 'a radio answering a bare pong, one lost: stalled, back at the next answer');
/* An err while a press waits for the radio's "tx 1" cannot say which command it answers. One that is not the PTT's
 * (a tune refused for a full queue, a Take control refused while on the air) leaves the press alone; the PTT's own
 * refusal ends it and sends the release, should this press have keyed meanwhile (review of C13) */
bare = false;
advance(6000);
ptt.press();
advance(10);
const p1 = now - 11;
ptt.err('freq: the radio is busy, try again');
ptt.err('busy: the radio is transmitting, control stays where it is');
check(ptt.state === 'requesting', 'a refused tune or Take control while the press waits: the press goes on');
ptt.radioTx(true);
advance(2000);
check(ptt.state === 'tx' && after(p1).filter(t => /^key PTT 1 \d+$/.test(t)).length >= 3 && !after(p1).includes('key PTT 0'), 'it keys and is renewed, no release sent');
ptt.release();
ptt.radioTx(false);
advance(6000);
ptt.press();
advance(10);
const p2 = now - 11;
check(ptt.err('controlled by the radio\'s operator: take control first') && ptt.state === 'refused', 'the PTT\'s own refusal while the press waits: refused');
check(after(p2).includes('key PTT 0'), 'and its release goes out (the press may have keyed)');
advance(3000);
check(!after(now - 2990).some(t => /^key PTT 1/.test(t)), 'no renewal after the refusal');
let micStops = 0;                                    /* the browser microphone, through the TCI */
const mptt = context.window.RadioPtt.create({el: null, send: text => link.send(text), session: {can: () => ''}, link,
  mic: {wanted: () => true, ready: () => true, start: () => true, stop: () => { micStops++; }}});
mptt.press();
mptt.err('view only: pair this browser to operate');
check(mptt.state === 'refused' && micStops === 1, 'with the browser microphone: the refusal stops the microphone\'s transmission too');
console.log(failures.length ? failures.length + ' C02 page check(s) failed' : 'C02 (fixed): the page lets go of a hold on a stalled link, numbers its presses and its pings; only the PTT\'s own refusal ends a press, with its release');
process.exit(failures.length ? 1 : 0);
})();
