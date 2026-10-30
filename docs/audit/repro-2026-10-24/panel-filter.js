/* Issue #91: the browser Panel's Filter pop-up against the radio's own filter rules. The Panel's production code
 * (popFilter() and what it calls, the passband on the panorama) runs on the page's markup; the radio is
 * filter-rules.cpp, the production SettingsManager with the remote server's own P_FILTER case, whose path is the
 * argument. In every mode (SAM and WFM: the X6200's), CW at the key tone's limits and default:
 * - each preset and each pair of edges typed into Low and High is sent, and the radio keeps exactly the edges
 *   sent, after a narrower filter and after a wider one (it checks each edge against the other as it sets it);
 * - with the radio's edges back, the presets lit are the ones whose edges they are: the one tapped, and none
 *   that only has the same width;
 * - the High field goes as far as the radio does, no further; Low is off where the radio keeps no low edge;
 * - a field emptied (as the browser empties one typed "3k") keeps the radio's edge;
 * - one edge typed alone comes out as the radio's own Filter low or Filter high would make it: in CW the other
 *   edge moves with it around the key tone (in the other modes it stays);
 * - the passband on the panorama: both sides in AM, NFM, SAM and WFM, as the radio's spectrum draws it.
 * node panel-filter.js <filter-rules binary> */
'use strict';
const path = require('path');
const {spawnSync} = require('child_process');
const {page, between} = require('./browser-dom.js');

const REPO = path.resolve(__dirname, '../../..');
const radioBin = process.argv[2];
if (!radioBin) { console.error('usage: node panel-filter.js <filter-rules binary>'); process.exit(2); }

const p = page('panel.html');
const src = p.source;
const has = marker => src.indexOf(marker) >= 0;
p.eval(`var state = null, sent = [];
  function set(name, v, v2) { sent.push(name + " " + v + (v2 !== undefined ? " " + v2 : "")); return true; }
  var $pop = $("#pop"), $box = $("#popbox");
  function popClose() { $pop.prop("hidden", true); $box.empty(); }`);
p.eval(between(src, '  const MODES = [', ';') + ';' +
       between(src, '  function fmtFilter(s) {', '\n  /* ---- Theme and layout') +
       between(src, '  /* The CW pitch', '  function hzToX') +
       between(src, '  function popFilter() {', '\n  /* ---- Controls ---- */'));
const MODES = p.eval('MODES');
const X6200 = m => m >= 8;
const name = (m, kt) => MODES[m] + (X6200(m) ? ' (X6200)' : '') + (m === 4 || m === 5 ? ' at a key tone of ' + kt + ' Hz' : '');

/* ---- The page ---- */
function open(st) {
  p.ctx.state = Object.assign({freq: 14074000}, st);
  p.ctx.popFilter();
  const box = p.doc.getElementById('popbox');
  const presets = box.querySelectorAll('.list button');
  const inputs = box.querySelectorAll('.lohi input');
  return {box, presets, lo: inputs[0], hi: inputs[1], set: box.querySelector('.lohi button')};
}
function sentBy(el) { p.ctx.sent = []; el.click(); return p.ctx.sent.slice(); }
/* What the page offers in a mode: each preset's label and what a tap sends; the fields; def: the radio's own
 * edges in the mode */
function offers(m, kt, def) {
  const st = {mode: m, kt, flo: 0, fhi: 1};
  const first = open(st);
  const out = {presets: [], typed: [], own: [], alone: [], lowOff: !!first.lo.disabled, highMax: first.hi.getAttribute('max')};
  for (let i = 0; i < first.presets.length; i++) {
    const o = open(st), b = o.presets[i];
    out.presets.push({label: b.textContent, cmd: sentBy(b)});
  }
  /* edges typed: past the radio's widest, the same twice, the wrong way round, and in CW off the key tone and an
   * odd width; Low only where the field takes it */
  const typed = [['', '99999'], ['3000', '3000'], ['2000', '1000'], ['300', '900'], ['650', '751'], ['', '3500']];
  for (const [lo, hi] of typed) {
    const o = open(st);
    if (lo !== '' && !o.lo.disabled) o.lo.value = lo;
    o.hi.value = hi;
    out.typed.push({label: 'Low ' + (lo === '' || o.lo.disabled ? '(as it was)' : lo) + ' High ' + hi, cmd: sentBy(o.set)});
  }
  /* from the radio's own edges: High emptied, Low emptied (where the field takes it), both; null: left as it was */
  for (const [lo, hi] of [[null, ''], ['', null], ['', '']]) {
    const o = open(def);
    if (lo !== null && o.lo.disabled) continue;
    if (lo !== null) o.lo.value = lo;
    if (hi !== null) o.hi.value = hi;
    out.own.push({label: 'from ' + def.flo + '-' + def.fhi + ', ' + [lo === null ? '' : 'Low', hi === null ? '' : 'High'].filter(Boolean).join(' and ') +
                  ' emptied', cmd: sentBy(o.set), want: [def.flo, def.fhi]});
  }
  /* from the radio's own edges, one edge typed: High wider, and in CW past twice a key tone of 400 and 700 Hz;
   * Low, where the field takes it, either way */
  const alone = [['high', def.fhi + 200]];
  if (m === 4 || m === 5) alone.push(['high', 2000]);
  if (!first.lo.disabled) alone.push(['low', def.flo + 100], ['low', def.flo - 100]);
  for (const [edge, v] of alone) {
    if (v < 0) continue;
    const o = open(def);
    (edge === 'low' ? o.lo : o.hi).value = String(v);
    out.alone.push({label: 'from ' + def.flo + '-' + def.fhi + ', ' + (edge === 'low' ? 'Low ' : 'High ') + v + ' alone',
                    cmd: sentBy(o.set), knob: edge + ' ' + v, def});
  }
  return out;
}
const edgesOf = cmd => cmd.length === 1 && /^filter \d+ \d+$/.test(cmd[0]) ? cmd[0].split(' ').slice(1).map(Number) : null;

/* ---- The radio ---- */
/* Lines to the stand-in, each answered with the radio's state after it */
function radio(lines) {
  const run = spawnSync(path.resolve(radioBin), {cwd: REPO, input: lines.join('\n') + '\n', encoding: 'utf8'});
  if (run.status !== 0) { console.error(run.stdout + run.stderr); throw new Error('the radio stand-in failed'); }
  const answers = run.stdout.trim().split('\n').map(l => l.split(' ').map(Number));
  if (answers.length !== lines.length) throw new Error('the radio answered ' + answers.length + ' of ' + lines.length + ' lines');
  return answers.map(a => ({mode: a[0], kt: a[1], flo: a[2], fhi: a[3]}));
}
const groups = [];
for (let m = 0; m < MODES.length; m++)
  for (const kt of (m === 4 || m === 5 ? [400, 700, 1200] : [700])) groups.push({m, kt});
/* the mode's own edges first, for the page to open with */
const defaults = radio(groups.flatMap(g => ['mode ' + g.m, 'kt ' + g.kt]));
groups.forEach((g, i) => { g.offer = offers(g.m, g.kt, defaults[2 * i + 1]); });
const lines = [], at = [];
const ask = (line, what) => { at.push(what); lines.push(line); };
for (const g of groups) {
  ask('mode ' + g.m, null); ask('kt ' + g.kt, {g, k: 'default'});
  ask('filter 0 99999', {g, k: 'widest'});
  ask('filter 500 600', {g, k: 'low'});
  /* the radio's own Filter low or high, from the mode's own edges */
  for (const o of g.offer.alone) { ask('filter ' + o.def.flo + ' ' + o.def.fhi, null); ask(o.knob, {g, o, k: 'knob'}); }
  for (const o of g.offer.presets.concat(g.offer.typed, g.offer.alone)) {
    const e = edgesOf(o.cmd);
    if (!e) continue;
    ask('filter 0 10', null); ask('filter ' + e.join(' '), {g, o, k: 'after narrower'});
    ask('filter 0 99999', null); ask('filter ' + e.join(' '), {g, o, k: 'after wider'});
  }
}
radio(lines).forEach((st, i) => {
  const w = at[i];
  if (!w) return;
  if (!w.o) { w.g[w.k] = st; return; }
  (w.o.radio = w.o.radio || {})[w.k] = st;
});

/* ---- The checks ---- */
const results = [];
function check(group, what, got, want) {
  const ok = JSON.stringify(got) === JSON.stringify(want);
  results.push({group, what, ok, got, want});
}
for (const g of groups) {
  const grp = name(g.m, g.kt), widest = g.widest;
  /* the fields: High as far as the radio goes; Low off where the radio keeps no low edge (asked 500-600) */
  check(grp, 'the High field\'s limit (the radio\'s widest: ' + widest.flo + '-' + widest.fhi + ')', g.offer.highMax, String(widest.fhi));
  check(grp, 'the Low field (the radio, asked 500-600, keeps ' + g.low.flo + '-' + g.low.fhi + ')', g.offer.lowOff ? 'off' : 'on', g.low.flo === 0 ? 'off' : 'on');
  const all = g.offer.presets.map(o => edgesOf(o.cmd));
  const litFor = st => { const o = open(st); return o.presets.map((b, i) => b.classList.contains('on') ? i : -1).filter(i => i >= 0); };
  const litWant = st => all.map((e, i) => e && e[0] === st.flo && e[1] === st.fhi ? i : -1).filter(i => i >= 0);
  check(grp, 'lit for the mode\'s own passband (' + g.default.flo + '-' + g.default.fhi + ')', litFor(g.default), litWant(g.default));
  for (const o of g.offer.presets.concat(g.offer.typed, g.offer.alone)) {
    const e = edgesOf(o.cmd), preset = g.offer.presets.indexOf(o);
    const what = (preset >= 0 ? 'preset ' + o.label : o.label) + ' sends ' + (e ? e.join('-') : JSON.stringify(o.cmd));
    if (!e) { check(grp, what, 'not sent', 'one "filter LO HI"'); continue; }
    for (const k of ['after narrower', 'after wider']) {
      const r = o.radio[k];
      check(grp, what + ', the radio keeps (' + k + ')', [r.flo, r.fhi], e);
    }
    const r = o.radio['after narrower'];
    const lit = litFor(r);
    check(grp, what + ', then lit', lit, litWant(r));
    if (preset >= 0) check(grp, 'preset ' + o.label + ' lit once tapped', lit.includes(preset), true);
  }
  for (const o of g.offer.own) check(grp, o.label + ' sends', edgesOf(o.cmd), o.want);
  for (const o of g.offer.alone) {
    const r = o.radio.knob;
    check(grp, o.label + ' sends what the radio\'s own Filter ' + o.knob.split(' ')[0] + ' makes of it (' + r.flo + '-' + r.fhi + ')',
          edgesOf(o.cmd), [r.flo, r.fhi]);
  }
  /* the passband around the dial, as src/spectrum_view.c draws it */
  const st = Object.assign({}, g.default), kt = st.kt;
  const want = g.m === 0 || g.m === 1 ? [-st.fhi, -st.flo] : g.m === 4 ? [st.flo - kt, st.fhi - kt] : g.m === 5 ? [kt - st.fhi, kt - st.flo]
             : g.m >= 6 ? [-st.fhi, st.fhi] : [st.flo, st.fhi];
  check(grp, 'the passband on the panorama at ' + st.flo + '-' + st.fhi, p.ctx.passband ? p.ctx.passband(st) : 'not in the page', want);
}

let bad = 0;
for (const g of groups) {
  const grp = name(g.m, g.kt), mine = results.filter(r => r.group === grp), wrong = mine.filter(r => !r.ok);
  bad += wrong.length;
  if (!wrong.length) {
    console.log('ok    ' + grp + ': presets ' + g.offer.presets.map(o => o.label + ' ' + (edgesOf(o.cmd) || []).join('-')).join(', ') +
                '; High to ' + g.offer.highMax + ', Low ' + (g.offer.lowOff ? 'off' : 'on') + ' (' + mine.length + ' cases)');
    continue;
  }
  for (const r of wrong) console.log('WRONG ' + grp + ', ' + r.what + ': ' + JSON.stringify(r.got) + '  (want ' + JSON.stringify(r.want) + ')');
}
console.log(bad ? bad + ' of ' + results.length + ' filter cases wrong (#91)' : 'All ' + results.length + ' filter cases as they should be (fixed, #91)');
process.exit(bad ? 1 : 0);
