/* C09: which keys reach the radio. The production keyboard code of the Panel and of the Screen page
 * (remote.html), with radio_session.js and its dialogs, runs on each page's own markup; the PTT, the
 * tuning and the key messages are counting stand-ins. A key the radio must not get is one aimed at a
 * focused control (Space or Enter press it), typed into a field, or pressed while a dialog is open. */
'use strict';
const {page, between, Event} = require('./browser-dom.js');

function panel() {
  const p = page('panel.html');
  p.run('static/js/radio_session.js');
  p.eval(`var counts = {press: 0, release: 0, tune: 0, vol: 0, sent: []};
    var ptt = {press: function() { counts.press++; }, release: function() { counts.release++; }};
    var state = {freq: 14074000, step: 10, vol: 20}, LVL = {vol: {max: 55}}, $pop = $("#pop");
    function tuneTo() { counts.tune++; }
    function lvlSend() { counts.vol++; }
    function send(s) { counts.sent.push(s); return true; }
    var ses = RadioSession.create({send: send, status: function() {}, el: document.getElementById("ses")});
    ses.role = "operate"; ses.ctl = {who: "browser", name: "other", mine: false};`);
  p.eval(between(p.source, '  /* ---- Keys (the Keys tab, PTT) ---- */', '  /* ---- Audio over TCI'));
  return p;
}
function remote() {
  const p = page('remote.html');
  p.run('static/js/radio_session.js');
  p.eval(`var counts = {press: 0, release: 0, sent: []};
    var ptt = {press: function() { counts.press++; }, release: function() { counts.release++; }};
    function send(s) { counts.sent.push(s); return true; }
    var ses = RadioSession.create({send: send, status: function() {}, el: document.getElementById("ses")});
    ses.role = "operate"; ses.ctl = {who: "browser", name: "other", mine: false};`);
  p.eval(between(p.source, '  /* ---- Keys ---- */', '  /* ---- Audio over TCI'));
  return p;
}
/* A key pressed and let go on what has the focus, counted from naught; the knob turns go out after their 30 ms */
function zero(p) { p.eval('counts.press = 0; counts.release = 0; counts.tune = 0; counts.vol = 0; counts.sent = [];'); }
function press(p, key, more) {
  zero(p);
  const down = p.key('keydown', key, null, more), up = p.key('keyup', key, null, more);
  p.timers.splice(0).forEach(t => t.fn());
  return {down, up, counts: JSON.parse(JSON.stringify(p.ctx.counts))};
}
function focus(p, el) { if (typeof el === 'string') el = p.doc.querySelector(el); el.focus(); if (p.doc.activeElement !== el) throw new Error('not focusable: ' + el.tagName); return el; }

const results = [];
function check(what, got, want) {
  const ok = Object.keys(want).every(k => JSON.stringify(got[k]) === JSON.stringify(want[k]));
  results.push({what, ok});
  console.log((ok ? 'ok    ' : 'WRONG ') + what + ': ' + Object.keys(want).map(k => k + '=' + JSON.stringify(got[k])).join(' ') + (ok ? '' : '  (want ' + Object.keys(want).map(k => k + '=' + JSON.stringify(want[k])).join(' ') + ')'));
}
const ptt = r => ({ptt: r.counts.press, prevented: r.down.defaultPrevented});

/* ---- Panel ---- */
for (const [sel, name] of [['#snd', 'Sound'], ['#theme', 'Light'], ['#appmode', 'App'], ['.tabs button[data-tab="log"]', 'the Log tab'], ['#b-pre', 'PRE']]) {
  const p = panel(); focus(p, sel);
  check('Panel, ' + name + ' focused, Space', ptt(press(p, ' ')), {ptt: 0, prevented: false});
}
{
  const p = panel(); p.ctx.ses.takeOpen();
  focus(p, p.doc.querySelectorAll('.spop button').pop());
  check('Panel, the session dialog\'s "Not now" focused, Space', ptt(press(p, ' ')), {ptt: 0, prevented: false});
  p.doc.activeElement.blur();
  check('Panel, nothing focused but the session dialog open, Space', ptt(press(p, ' ')), {ptt: 0});
}
{
  const p = panel(); p.ctx.ses.pairOpen();
  check('Panel, the pairing dialog\'s code field, Space', ptt(press(p, ' ')), {ptt: 0});
}
{
  const p = panel();
  const sel = p.$("<select><option>a</option><option>b</option></select>").appendTo(p.doc.body)[0];
  const ed = p.$("<div contenteditable>notes</div>").appendTo(p.doc.body)[0];
  ed.focus();
  check('Panel, a contenteditable element focused, Space', ptt(press(p, ' ')), {ptt: 0, prevented: false});
  focus(p, sel);
  let r = press(p, ' ');
  check('Panel, a select focused, Space', ptt(r), {ptt: 0, prevented: false});
  r = press(p, 'ArrowRight');
  check('Panel, a select focused, the right arrow', {tune: r.counts.tune, prevented: r.down.defaultPrevented}, {tune: 0, prevented: false});
}
{
  const p = panel(); p.$('#pop').prop('hidden', false); p.doc.activeElement.blur();
  check('Panel, its own pop-up open, Space', ptt(press(p, ' ')), {ptt: 0});
}
{
  const p = panel();
  let r = press(p, ' ');
  check('Panel, nothing focused, Space (PTT as before)', {ptt: r.counts.press, released: r.counts.release, prevented: r.down.defaultPrevented}, {ptt: 1, released: 1, prevented: true});
  r = press(p, 'ArrowRight');
  check('Panel, nothing focused, the right arrow tunes', {tune: r.counts.tune}, {tune: 1});
  focus(p, '#snd');
  r = press(p, 'ArrowRight');
  check('Panel, Sound focused, the right arrow still tunes (a button takes no arrows)', {tune: r.counts.tune}, {tune: 1});
  focus(p, '#pttbtn'); zero(p);
  const down = p.key('keydown', ' '); p.key('keydown', ' ', null, {repeat: true}); p.key('keyup', ' ');
  check('Panel, the PTT button focused, Space held', {ptt: p.ctx.counts.press, released: p.ctx.counts.release, prevented: down.defaultPrevented}, {ptt: 1, released: 1, prevented: true});
}
{
  /* A click with the mouse leaves the keyboard where it was (Safari does so for every button): Space after
   * a click on Sound is PTT, as before; a button reached with Tab keeps Space for itself */
  const p = panel();
  const md = new Event('mousedown'); p.doc.querySelector('#snd').dispatchEvent(md);
  check('Panel, a mouse press on Sound takes no focus', {prevented: md.defaultPrevented}, {prevented: true});
  const mi = new Event('mousedown'); p.$("<input id='x'>").appendTo(p.doc.body)[0].dispatchEvent(mi);
  check('Panel, a mouse press on a field focuses it as ever', {prevented: mi.defaultPrevented}, {prevented: false});
  /* base.html's menu opens on :focus-within: a tap must still focus its group button */
  const mm = new Event('mousedown'); p.$("<nav><div class='grp'><button type='button'>Remote</button></div></nav>").prependTo(p.doc.body).find('button')[0].dispatchEvent(mm);
  check('Panel, a mouse press on the menu at the top focuses it as ever', {prevented: mm.defaultPrevented}, {prevented: false});
}
/* A mouse press on a panel button gives the keyboard back to the page, whatever had it: the menu at the
 * top closes (it is open while it has the focus), a field is left, and Space is PTT */
const mouseDown = (p, sel) => { const ev = new Event('mousedown'); p.doc.querySelector(sel).dispatchEvent(ev); return ev; };
const active = p => p.doc.activeElement === p.doc.body ? 'body' : p.doc.activeElement.id || p.doc.activeElement.tagName;
{
  const p = panel();
  p.$("<nav><div class='grp'><button type='button' id='navbtn'>Remote</button></div></nav>").prependTo(p.doc.body);
  focus(p, '#navbtn');
  mouseDown(p, '#snd');
  const r = press(p, ' ');
  check('Panel, the menu at the top open, a click on Sound, then Space', {focus: active(p), ptt: r.counts.press}, {focus: 'body', ptt: 1});
}
{
  const p = panel();
  focus(p, '#lg-call');
  mouseDown(p, '#lg-log');
  const r = press(p, ' ');
  check('Panel, a call typed, a click on LOG, then Space', {focus: active(p), ptt: r.counts.press}, {focus: 'body', ptt: 1});
}
{
  const p = panel();
  focus(p, '#snd');
  mouseDown(p, '#theme');
  const r = press(p, ' ');
  check('Panel, Sound reached with Tab, a click on Light, then Space', {focus: active(p), ptt: r.counts.press}, {focus: 'body', ptt: 1});
}

/* ---- Screen page (remote.html) ---- */
for (const [sel, name] of [['[data-key="GEN"]', 'GEN'], ['#snd', 'Sound'], ['#full', 'Fullscreen'], ['.ses', 'the pairing button']]) {
  const p = remote(); focus(p, sel);
  const r = press(p, 'Enter');
  check('Screen, ' + name + ' focused, Enter', {mfk: r.counts.sent.filter(s => /^key MFK/.test(s)).length, prevented: r.down.defaultPrevented}, {mfk: 0, prevented: false});
  const s = press(p, ' ');
  check('Screen, ' + name + ' focused, Space', ptt(s), {ptt: 0, prevented: false});
}
{
  const p = remote(); focus(p, '#pic');
  let r = press(p, 'ArrowRight');
  check('Screen, the picture select focused, the right arrow', {sent: r.counts.sent, prevented: r.down.defaultPrevented}, {sent: [], prevented: false});
  r = press(p, 'Enter');
  check('Screen, the picture select focused, Enter', {sent: r.counts.sent}, {sent: []});
}
{
  const p = remote(); p.ctx.ses.sessionsOpen(); p.doc.activeElement.blur();
  const got = ['F1', 'Enter', ' '].map(k => press(p, k).counts);
  check('Screen, the sessions dialog open, F1, Enter, Space', {sent: got.flatMap(c => c.sent), ptt: got[2].press}, {sent: [], ptt: 0});
}
{
  const p = remote();
  let r = press(p, 'Enter');
  check('Screen, nothing focused, Enter pushes the MFK (as before)', {sent: r.counts.sent, prevented: r.down.defaultPrevented}, {sent: ['key MFK 1', 'key MFK 0'], prevented: true});
  r = press(p, 'F2');
  check('Screen, nothing focused, F2', {sent: r.counts.sent}, {sent: ['key F2 1', 'key F2 0']});
  r = press(p, 'ArrowLeft', {shiftKey: true});
  check('Screen, nothing focused, Shift + the left arrow', {sent: r.counts.sent}, {sent: ['knob main -10']});
  r = press(p, ' ');
  check('Screen, nothing focused, Space (PTT as before)', ptt(r), {ptt: 1, prevented: true});
  focus(p, '#pttbtn');
  r = press(p, ' ');
  check('Screen, the PTT button focused, Space', ptt(r), {ptt: 1, prevented: true});
  r = press(p, 'Enter');
  check('Screen, the PTT button focused, Enter is not the MFK', {sent: r.counts.sent}, {sent: []});
  const md = new Event('mousedown'); p.doc.querySelector('#snd').dispatchEvent(md);
  check('Screen, a mouse press on Sound takes no focus', {prevented: md.defaultPrevented}, {prevented: true});
}

{
  const p = remote();
  p.$("<nav><div class='grp'><button type='button' id='navbtn'>Remote</button></div></nav>").prependTo(p.doc.body);
  focus(p, '#navbtn');
  mouseDown(p, '[data-key="GEN"]');
  const r = press(p, 'Enter');
  check('Screen, the menu at the top open, a click on GEN, then Enter', {focus: active(p), sent: r.counts.sent}, {focus: 'body', sent: ['key MFK 1', 'key MFK 0']});
}
/* The picture list picked with the mouse gives the keyboard back to the radio; reached with Tab it keeps it */
function picture() {
  const p = remote();
  p.eval(between(p.source, '  /* The picture asked for', '  const screenEl'));
  return p;
}
{
  const p = picture();
  const pic = focus(p, '#pic');
  pic.dispatchEvent(new Event('mousedown'));
  p.$(pic).val('fast'); pic.dispatchEvent(new Event('change'));
  const sent = p.ctx.counts.sent.slice();
  const r = press(p, 'ArrowRight'), f = press(p, 'F1');
  check('Screen, the picture picked with the mouse, then the right arrow and F1', {picture: sent, focus: active(p), sent: r.counts.sent.concat(f.counts.sent)},
        {picture: ['picture fast'], focus: 'body', sent: ['knob main 1', 'key F1 1', 'key F1 0']});
}
{
  const p = picture();
  const pic = focus(p, '#pic');
  p.key('keydown', 'ArrowDown', pic);
  p.$(pic).val('fast'); pic.dispatchEvent(new Event('change'));
  const r = press(p, 'ArrowRight');
  check('Screen, the picture picked with the keyboard: the list keeps the keys', {focus: active(p), sent: r.counts.sent}, {focus: 'pic', sent: []});
}

const bad = results.filter(r => !r.ok).length;
console.log(bad ? bad + ' of ' + results.length + ' keyboard cases wrong (audit C09)' : 'All ' + results.length + ' keyboard cases as they should be (fixed, audit C09)');
process.exit(bad ? 1 : 0);
