// Issue #85, TUNE for an external tuner from the Panel, the page's side: the production RadioLink and RadioTune with a
// fake socket and clock. A press, not a hold: "tune 1 N" while the tune is on, renewed every second with its own
// number, "tune 0" to stop it; a link that stops delivering ends the renewals (the radio stops the tune three
// seconds after the last one: remote-scenarios.c, "tune"), and a press while the radio does not answer sends
// nothing. The radio's refusals and stops name the tune and end it here; another err leaves it. A tune the radio
// did not start within 2 s is let go; one started at the radio shows, and a press stops it.
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
let socket = null, paused = false;
function FakeSocket() { socket = this; this.readyState = 0; add(() => { this.readyState = 1; this.onopen(); }, 10, false); }
FakeSocket.prototype.send = function(text) {
  sent.push({at: now, text});
  const ping = /^ping(?: (\d+))?$/.exec(text);
  if (!ping || paused) return;
  add(() => this.onmessage({data: ping[1] ? 'pong ' + ping[1] : 'pong'}), 20, false);
};
FakeSocket.prototype.close = function() {};
const context = {window: {}, Date: {now: () => now}, WebSocket: FakeSocket, Promise,
  setTimeout: (fn, ms) => add(fn, ms, false), setInterval: (fn, ms) => add(fn, ms, true), clearTimeout: id => timers.delete(id), clearInterval: id => timers.delete(id)};
for (const f of ['radio_link.js', 'radio_session.js'])
  vm.runInNewContext(fs.readFileSync('webserver/src/x6100_webserver/static/js/' + f, 'utf8'), context);
let tune = null, can = '', stallAt = 0;
const shown = [];
const link = context.window.RadioLink.create({url: () => 'ws://radio/', ping: 'ping',
  onStall: on => { if (on) { stallAt = now; tune.lost(); } }});               /* as the Panel does (letGo) */
tune = context.window.RadioTune.create({el: null, send: text => link.send(text), status: t => shown.push(t),
                                        session: {can: () => can}, link});
const after = t => sent.filter(x => x.at > t).map(x => x.text);
const failures = [];
function check(ok, what) { console.log((ok ? 'ok    ' : 'FAIL  ') + what); if (!ok) failures.push(what); }

(async () => {
link.connect();
await new Promise(r => setImmediate(r));             /* the URL comes as a promise */
advance(100);

let t0 = now;
tune.press();
advance(200);
tune.radio(true, 15);                                /* the radio's state: tune 1, tune_s 15 */
advance(2000);
check(tune.state === 'on' && sent.filter(x => x.text === 'tune 1 1').length === 3,
      'a press: tune 1 1, renewed every second while the tune is on (' + after(t0).filter(t => /^tune/.test(t)).join(', ') + ')');
paused = true;                                       /* nothing comes back */
advance(9000);
check(link.stalled && tune.state === 'ended', 'the link stalls (a ping unanswered for 3 s): the tune is over here, the button says so');
check(stallAt && !after(stallAt).some(t => /^tune 1/.test(t)), 'no renewal once the stall is seen: the radio stops it 3 s after the last it got');
check(sent.some(x => x.at === stallAt && x.text === 'tune 0'),
      'and the stop sent behind the renewals stuck in the link (tune 0): delivered late, they end with it, as a PTT\'s release');
tune.press();
check(tune.state === 'refused' && !after(now - 1).some(t => /^tune 1/.test(t)), 'a press while the radio does not answer: refused, nothing sent');
paused = false;
socket.onmessage({data: 'state {}'});
advance(100);
check(!link.stalled, 'the link answers again');
advance(6000);

t0 = now;
tune.press();
check(after(t0 - 1).includes('tune 1 2'), 'the next press has a number of its own: tune 1 2');
advance(100);
tune.radio(true, 15);
advance(1000);
tune.radio(false, 15);                               /* its limit, at the radio */
advance(3000);
check(tune.state === 'ended' && !after(now - 2500).some(t => /^tune 1/.test(t)), 'the radio ended it: over here, no more renewals');
advance(6000);

tune.press();
advance(100);
tune.radio(true, 15);
advance(500);
t0 = now;
tune.press();
check(after(t0 - 1).includes('tune 0') && tune.state === 'stopping', 'a press while it is on: tune 0, stopping');
advance(1100);
check(!after(t0).some(t => /^tune 1/.test(t)), 'no renewal after the stop');
tune.radio(false, 15);
check(tune.state === 'idle', 'the radio confirms: idle');

t0 = now;
tune.press();
advance(100);
const mine = tune.err('tune: on the air'), other = tune.err('freq: locked (LOCK, or an app holds the frequency)');
advance(1500);
check(mine && !other && tune.state === 'refused' && !after(t0 + 100).some(t => /^tune 1/.test(t)),
      'the radio refuses it ("tune: on the air"): refused, no renewal; another err is not the tune\'s');
advance(6000);
tune.press();
advance(200);
check(tune.err('view only: pair this browser to operate') && tune.state === 'refused', 'an access refusal while the press waits: the tune\'s');
advance(6000);
check(!tune.err('view only: pair this browser to operate'), 'the same with no press waiting: not the tune\'s');
tune.press();
advance(200);
tune.radio(true, 15);
advance(300);
check(tune.err('tune stopped: no renewal from the page') && tune.state === 'ended' && !tune.radioOn,
      'the radio stopped it ("tune stopped ..."): over here, and not taken for one still on');
advance(6000);

t0 = now;
tune.press();
advance(4100);                                       /* the radio never says tune 1 */
const letGoAt = (sent.find(x => x.at > t0 && x.text === 'tune 0') || {}).at;
check(tune.state === 'refused' && letGoAt - t0 === 2000 && !after(letGoAt).some(t => /^tune 1/.test(t)),
      'not started within 2 s: let go (tune 0), refused, no renewal after');
advance(6000);

tune.radio(true, 20);                                /* started at the radio, its key or button */
advance(3000);
t0 = now;
tune.press();
check(after(t0 - 1).includes('tune 0') && !after(t0 - 1).some(t => /^tune 1/.test(t)), 'a tune started at the radio: a press stops it (tune 0)');
tune.radio(false, 20);
advance(6000);

can = 'view only: pair this browser to operate';
t0 = now;
tune.press();
check(tune.state === 'refused' && !after(t0 - 1).some(t => /^tune/.test(t)), 'a browser that may not operate: refused, nothing sent');

console.log(failures.length ? failures.length + ' tune page cases FAILED' : 'All tune page cases as they should be (#85)');
process.exit(failures.length ? 1 : 0);
})();
