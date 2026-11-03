/* Issue #58 (a report from a test over Tailscale Funnel): the status line of the Panel and of the Screen page says how
 * things are only while they are so. Control taken at the radio, a touch on the waterfall ("Take control first"),
 * Take control: the refusal stayed up although this browser had control again. The pages' own code (their status
 * line, act and mayAct, onText, the session, PTT and TUNE as they make them, the stall, the second's tick) with the
 * production radio_session.js on each page's own markup; the radio a stand-in that answers as remote_server.c does.
 * Each "how things are" text goes once it is not so: control moved, the radio answering again, back on receive, the
 * tune over, LOCK off, the radio no longer on the air; news ("QSO logged") stays. */
'use strict';
const {page, between} = require('./browser-dom.js');

const results = [];
function check(what, ok, got) {
  results.push({what, ok});
  console.log((ok ? 'ok    ' : 'WRONG ') + what + (got !== undefined ? ': ' + JSON.stringify(got) : ''));
}

/* What both pages need around their own code: the link to the radio, what it was sent, the page's tick */
function stand(p) {
  const ticks = [];
  p.ctx.setInterval = fn => { ticks.push(fn); return ticks.length; };
  p.eval(`var stHolds = null, FUNNEL = false, secure = false, conf = {enabled: true, tci_enabled: true}, sent = [];
    var link = {up: true, stalled: false, gen: 1, send: function(s) { sent.push(s); return true; }, close: function() {}, tryNow: function() {}, retryIn: function() { return 0; }};
    var tlink = {up: false, send: function() { return false; }, tryNow: function() {}, close: function() {}};
    var micOn = false, txOn = false, micStream = null, micState = "off", greeted = true, awayless = false, unreachable = false, updating = null;
    var held = {}, lastFrame = Date.now(), frames = 0, bytes = 0;
    function showHealth() {} function view() {} function txStart() { return false; } function txStop() {}`);
  return {
    st: () => p.doc.getElementById('st').textContent,
    error: () => p.doc.getElementById('st').classList.contains('error'),
    dialog: () => { const b = p.doc.querySelector('.spop'); return b && !b.hasAttribute('hidden') ? b.querySelector('.sbox').textContent : ''; },
    button: (scope, text) => [...p.doc.querySelectorAll(scope + ' button')].find(b => b.textContent === text),
    ticks,
    sent: () => p.ctx.sent.splice(0),
  };
}

function panel() {
  const p = page('panel.html');
  p.run('static/js/radio_session.js');
  const t = stand(p);
  const src = p.source;
  p.eval(`var $st = $("#st"), radio = document.querySelector(".pn-radio"), tuneWant = null, tuneTimer = null, want = {};
    var state = null, stateAt = 0, ses = null, ptt = null, xtune = null;
    function showState() { if (xtune && state) xtune.radio(!!state.tune, state.tune_s); }
    function requestDraw() {} function logTick() {} function logReconcile() {} function fmtFreq(hz) { return String(hz); }
    function wantDrop() { want = {}; } function keyUp(k) { delete held[k]; }`);
  p.eval(between(src, '  function status(', '  /* An https page'));
  p.eval(between(src, '  function send(s) {', '  function set(name, v, v2)'));
  p.eval(between(src, '  /* May the page ask for a change', '  function showState() {'));
  p.eval(between(src, '  function tuneTo(hz) {', '  function roundStep(hz)'));
  p.eval(between(src, '  function onText(t) {', '  link = RadioLink.create({'));
  p.eval(between(src, '  /* Who this browser is to the radio and who controls it', '  /* What touches the air is let go'));
  p.eval(between(src, '  function letGo(why) {', '  function stateLine()'));
  const n = t.ticks.length;
  p.eval(between(src, '  setInterval(function() { showHealth(); view(); logTick();', '\n'));
  t.tick = t.ticks[n];                                     /* the page's own every second */
  t.text = (s) => p.eval('onText(' + JSON.stringify(s) + ')');
  return {p, ...t};
}

function screen() {
  const p = page('remote.html');
  p.run('static/js/radio_session.js');
  const t = stand(p);
  const src = p.source;
  p.eval(`var $st = $("#st"), screenEl = document.getElementById("screen"), panelEl = document.querySelector(".rm-panel"), knobAcc = {};
    var ses = null, ptt = null;
    function keyUp(k) { delete held[k]; } function keysHave() {}`);
  p.eval(between(src, '  function status(', '  function wsUrl(kind) {'));
  p.eval(between(src, '  function send(s) {', '  /* The picture asked for'));
  p.eval('var pageOnText = ({' + between(src, '    onText: function(t) {', '    onBinary: showFrame,') + '}).onText;');
  p.eval(between(src, '  /* Who this browser is to the radio and who controls it', '  /* What touches the air is let go'));
  p.eval(between(src, '  function letGo(why) {', '  function pictureLine()'));
  const n = t.ticks.length;
  p.eval(between(src, '  setInterval(function() { showHealth(); view();', '\n'));
  t.tick = t.ticks[n];
  t.text = (s) => p.eval('pageOnText(' + JSON.stringify(s) + ')');
  return {p, ...t};
}

/* This browser paired, in control; the radio then hands control to its own keys (a knob turned at the radio) */
function inControl(g) {
  g.text('role operate 1 Firat');
  g.text('ctl browser Firat 1');
  g.sent();
}
const TAKEN = 'Control taken at the radio: Take control to operate from here again';
const FIRST = 'The radio is operated at the radio: Take control first';

/* ---- the Panel: the report ---- */
{
  const g = panel();
  inControl(g);
  g.text('ctl local 0');
  check('Panel: control taken at the radio, the status line says so', g.st() === TAKEN && g.error(), g.st());
  g.p.eval('mayAct()');                                  /* a touch on the waterfall */
  check('a touch on the waterfall: "Take control first", and the Take control dialog', g.st() === FIRST && /^Take control/.test(g.dialog()), g.st());
  g.p.$(g.button('.spop', 'Take control')).trigger('click');
  check('the dialog\'s Take control asks the radio', JSON.stringify(g.sent()) === '["control take"]');
  g.text('ctl browser Firat 1');                          /* the radio's answer */
  check('control back here: "Take control first" gone at once', !/Take control/.test(g.st()), g.st());
  check('the status line says this browser is in control, not as an error', g.st() === 'In control: this browser operates the radio' && !g.error(), g.st());
  g.tick();
  check('and the second\'s tick leaves that', g.st() === 'In control: this browser operates the radio', g.st());
}
{
  const g = panel();
  inControl(g);
  g.text('ctl local 0');
  g.p.$(g.button('#ses', 'At the radio · Take control')).trigger('click');
  check('Panel: "At the radio · Take control" asks the radio', JSON.stringify(g.sent()) === '["control take"]');
  g.text('ctl browser Firat 1');
  check('control back: "Control taken at the radio" gone', !/Control taken|Take control/.test(g.st()), g.st());
}
{
  /* a key on the Keys tab, sent while the page still took control for free: the radio's own refusal */
  const g = panel();
  inControl(g);
  g.text('ctl none 0');
  g.text('err controlled by the radio\'s operator: take control first');
  g.text('ctl local 0');
  check('Panel: the radio\'s refusal shown', g.st() === 'controlled by the radio\'s operator: take control first', g.st());
  g.p.eval('ses.ctlClick()');
  g.text('ctl browser Firat 1');
  check('control back: the radio\'s refusal gone', !/take control/i.test(g.st()), g.st());
}
{
  const g = panel();
  inControl(g);
  g.text('ctl browser Kitchen 0');
  check('Panel: control taken by another browser', g.st() === 'Control taken by Kitchen', g.st());
  g.text('ctl none 0');                                    /* Kitchen released */
  check('that browser released: "Control taken by Kitchen" gone', g.st() === '', g.st());
}
{
  const g = panel();
  inControl(g);
  g.text('ctl local 0');
  g.p.eval('ptt.press()');
  check('Panel: PTT refused while operated at the radio', g.st() === FIRST, g.st());
  g.p.eval('ses.ctlClick()');
  g.text('ctl browser Firat 1');
  check('control back: the PTT\'s refusal gone from the status line', !/Take control/.test(g.st()), g.st());
}
{
  /* control free as far as the page knew: the radio refuses the press, then says who has control */
  const g = panel();
  inControl(g);
  g.text('ctl none 0');
  g.p.eval('ptt.press()');
  g.text('err controlled by the radio\'s operator: take control first');
  g.text('ctl local 0');
  check('Panel: a press refused by the radio, its refusal shown', g.st() === 'controlled by the radio\'s operator: take control first', g.st());
  g.p.eval('ses.ctlClick()');
  g.text('ctl browser Firat 1');
  check('control back: the radio\'s refusal of the press gone', !/take control/i.test(g.st()), g.st());
}
{
  const g = panel();
  inControl(g);
  g.p.eval('ptt.press()');
  g.text('tx 1');
  check('Panel: on the air', g.st() === 'On the air (the radio\'s microphone)', g.st());
  g.p.eval('ptt.release()');
  g.text('tx 0');
  g.tick();
  check('released and back on receive: "On the air" gone', g.st() === '', g.st());
}
{
  const g = panel();
  inControl(g);
  g.p.eval('xtune.press()');
  g.text('state ' + JSON.stringify({freq: 7030000, tune: 1, tune_s: 15}));
  check('Panel: TUNE on the air', /^TUNE on the air/.test(g.st()), g.st());
  g.p.eval('xtune.press()');                               /* stop */
  g.text('state ' + JSON.stringify({freq: 7030000, tune: 0, tune_s: 15}));
  g.tick();
  check('the tune stopped here: "TUNE on the air" gone', g.st() === '', g.st());
}
{
  const g = panel();
  inControl(g);
  g.text('state ' + JSON.stringify({freq: 7030000, lock: 1}));
  g.p.eval('tuneTo(7031000)');
  check('Panel: LOCK on, a tune refused', /^LOCK is on/.test(g.st()), g.st());
  g.text('state ' + JSON.stringify({freq: 7030000, lock: 0}));
  g.tick();
  check('LOCK off: its text gone', g.st() === '', g.st());
}
{
  const g = panel();
  inControl(g);
  g.p.eval('held.F1 = true; link.stalled = true; stalled(true)');
  check('Panel: the radio silent, keys let go', /^The radio does not answer/.test(g.st()), g.st());
  g.p.eval('link.stalled = false; stalled(false)');
  g.tick();
  check('the radio answers again: "does not answer" gone', g.st() === '', g.st());
  g.p.eval('link.stalled = true; act("set nr 1")');
  g.p.eval('link.stalled = false');
  g.tick();
  check('a change refused while silent: gone when it answers', g.st() === '', g.st());
}
{
  const g = panel();
  inControl(g);
  g.text('tx 1');                                          /* another place on the air */
  g.text('ctl local 0');
  g.p.eval('ses.ctlClick()');
  check('Panel: Take control while the radio transmits, refused here', /^The radio is transmitting/.test(g.st()), g.st());
  g.text('tx 0');
  g.tick();
  check('the radio back on receive: that text gone', g.st() === '', g.st());
}
{
  const g = panel();
  inControl(g);
  g.text('ctl none 0');                                    /* released here */
  g.text('logged ok');
  g.text('ctl local 0');
  g.text('ctl browser Kitchen 0');
  g.tick();
  check('Panel: news stays ("QSO logged" through changes of control)', g.st() === 'QSO logged', g.st());
}

/* ---- the Screen page ---- */
{
  const g = screen();
  inControl(g);
  g.text('ctl local 0');
  check('Screen: control taken at the radio, the status line says so', g.st() === TAKEN, g.st());
  g.text('err controlled by the radio\'s operator: take control first');     /* a key or a knob refused */
  check('a key refused by the radio: its refusal, and the Take control dialog', g.st() === 'controlled by the radio\'s operator: take control first' &&
        /^Take control/.test(g.dialog()), g.st());
  g.p.$(g.button('.spop', 'Take control')).trigger('click');
  g.text('ctl browser Firat 1');
  check('control back: the refusal gone, in control', g.st() === 'In control: this browser operates the radio', g.st());
}
{
  const g = screen();
  inControl(g);
  g.p.eval('ptt.press()');
  g.text('tx 1');
  g.p.eval('ptt.release()');
  g.text('tx 0');
  g.tick();
  check('Screen: released and back on receive: "On the air" gone', g.st() === '', g.st());
  g.p.eval('held.F1 = true; link.stalled = true; stalled(true)');
  g.p.eval('link.stalled = false; stalled(false)');
  g.tick();
  check('Screen: the radio answers again: "does not answer" gone', g.st() === '', g.st());
}
{
  const g = screen();
  g.text('role view 0 -');
  g.text('err view only: pair this browser to operate');
  check('Screen: a key from a browser that only watches, refused', /^view only/.test(g.st()), g.st());
  g.text('paired 0123456789abcdef0123456789abcdef operate 1');
  g.text('role operate 1 Firat');
  g.tick();
  check('paired: the refusal gone ("Paired as" said)', /^Paired as/.test(g.st()), g.st());
}

const bad = results.filter(r => !r.ok).length;
console.log(bad ? bad + ' status line case(s) WRONG' : 'All ' + results.length + ' status line cases as they should be (#58)');
process.exitCode = bad ? 1 : 0;
