#!/usr/bin/env node
/* The web Weather page's own script (webserver/.../views/meteo.html) under Node, on the small DOM of the audit's
 * browser-dom.js, for Weather units (#315):
 * - wx() on each case of units_cases.txt, which meteo_c_test.c checks src/meteo.c with: the page and the radio's
 *   screen show the same numbers;
 * - the page drawn from one answer (metric, as the helper writes it) in metric and in US units: the weather now, its
 *   tiles, the five days, a warning worked out from the forecast;
 * - Units: shown at once and set on the radio (/api/display, meteo_units), back as it was when the radio does not
 *   take it; each answer brings the radio's own setting, but not one asked before a change made here.
 * Prints PAGE OK, or exits 1 after the failures. Run by the Makefile, or `node meteo_page_test.js` */
'use strict';
const fs = require('fs');
const path = require('path');
const {page, between} = require('../../docs/audit/repro-2026-10-24/browser-dom.js');

let failed = 0;
function check(what, ok, got) {
  if (!ok) { failed++; console.log('FAIL: ' + what + (got !== undefined ? ' (' + got + ')' : '')); }
}

const p = page('meteo.html');
p.eval(between(p.script, 'const mt = ', '\nupdate(true);'));
/* The DOM's $.ajax takes a JSON body and leaves out complete: the GET's data as JSON, complete after each answer */
p.eval(`(function () {
  const real = $.ajax;
  $.ajax = function (o) {
    const then = (fn) => function () { if (fn) fn.apply(this, arguments); if (o.complete) o.complete(); };
    return real(Object.assign({}, o, {data: o.data && typeof o.data === "object" ? JSON.stringify(o.data) : o.data,
                                      success: then(o.success), error: then(o.error)}));
  };
})()`);
const $ = p.$;

/* ---- the cases shared with the C test ---- */
let n = 0;
for (const line of fs.readFileSync(path.join(__dirname, 'units_cases.txt'), 'utf8').split('\n')) {
  if (!line || line[0] === '#') continue;
  const m = /^(\S+) (\S+) (\S+) (\S+) (\S+) (.*)$/.exec(line);
  if (!m) { check('a case of units_cases.txt', false, line); continue; }
  const got = p.eval(`mt.us = ${m[1] === 'us'}; wx(${Number(m[3])}, ${JSON.stringify(m[2])}, ${Number(m[4])}, ` +
                     `${m[5] === 'unit' ? 'undefined' : JSON.stringify(m[5])})`);
  check(line, got === m[6], got);
  n++;
}
check('the cases read', n >= 30, n);
p.eval('mt.us = false');

/* ---- the page drawn, metric and US ---- */
const ANSWER = {
  place: 'Brussels, Brussels-Capital, Belgium', where: 'GPS', updated: '13:45', tz: 'Europe/Brussels', tz_abbr: 'CET',
  now: {text: 'Light rain', temp: 9.4, feels: 6.9, dew: 6.3, press: 1008.6, wind_kmh: 24.5, wind_dir: 228,
        gust_kmh: 52.9, hum: 81, cloud: 96, uv: 0.55, vis_km: 18.2},
  days: [{date: '2026-11-16', text: 'Light rain', tmin: 6.1, tmax: 10.8, prob: 85, rain_mm: 6.4, wind_max: 31.2,
          gust_max: 68.4, wind_dir: 225, uv: 0.9, rise: '08:04', set: '16:46'}],
  sun: {rise: '08:04', set: '16:46'}, alerts_src: 'forecast', credits: [], errors: [], notices: false,
  alerts: [{src: 'forecast', kind: 'computed', level: 3, calc: 'gust', value: 95, day: 0, mine: true}]};

function answer(us) {
  const req = $.requests.filter((r) => r.url === '/api/meteo' && !r.done).pop();
  check('the page asked for the weather', !!req);
  if (req) req.ok(Object.assign(JSON.parse(JSON.stringify(ANSWER)), {us: us}));
}
function shown() {
  return {temp: $('#mt-temp').text(), feels: $('#mt-feels').text(), lohi: $('#mt-lohi').text(),
          tiles: [...$('#mt-tiles b')].map((b) => b.textContent).join(' | '),
          day: [...$('#mt-days tbody td')].map((td) => td.textContent).join(' | '),
          warn: $('#mt-warn h4').text(), units: $('#mt-units').val()};
}
function same(what, got, want) {
  for (const k of Object.keys(want)) check(what + ': ' + k, got[k] === want[k], JSON.stringify(got[k]));
}
const METRIC = {temp: '9.4 °C', feels: 'Feels like 7 °C', lohi: 'Low 6°, high 11 °C',
  tiles: 'SW 25 km/h | 53 km/h | 81 % | 1009 hPa | 85 %, 6.4 mm | 96 % | 6.3 °C | 18 km | 1, low',
  day: 'Today 16 | Light rain | 11 °C | 6 °C | 85 %, 6.4 mm | SW 31 km/h | 68 km/h | 1 | 08:04 to 16:46',
  warn: 'Orange: gusts up to 95 km/h, from the forecast  (here)', units: '0'};
const US = {temp: '49 °F', feels: 'Feels like 44 °F', lohi: 'Low 43°, high 51 °F',
  tiles: 'SW 15 mph | 33 mph | 81 % | 29.78 inHg | 85 %, 0.25 in | 96 % | 43 °F | 11 mi | 1, low',
  day: 'Today 16 | Light rain | 51 °F | 43 °F | 85 %, 0.25 in | SW 19 mph | 43 mph | 1 | 08:04 to 16:46',
  warn: 'Orange: gusts up to 59 mph, from the forecast  (here)', units: '1'};

p.eval('update(true)');
answer(false);
same('metric', shown(), METRIC);
p.eval('update(true)');
answer(true);
same('the radio in US units', shown(), US);

/* ---- Units set here ---- */
function posted() {
  const req = $.requests.filter((r) => r.url === '/api/display' && !r.done).pop();
  check('Units sent to the radio', !!req);
  return req || {body: {}, ok: () => {}, fail: () => {}};
}
$('#mt-units').val('0').trigger('change');
same('Metric chosen: at once', shown(), METRIC);
let req = posted();
check('as the Display page sets it', req.type === 'POST' && req.body.name === 'meteo_units' && req.body.val === 0,
      JSON.stringify(req.body));
req.ok({status: 'OK'});
check('the status says so', $('#mt-status').text() === 'Metric units: set on the radio too', $('#mt-status').text());

$('#mt-units').val('1').trigger('change');
same('US chosen: at once', shown(), Object.assign({}, US));
req = posted();
check('US sent', req.body.val === 1, JSON.stringify(req.body));
req.fail(409, 'error', {status: 'error', msg: 'Unknown option'});
same('not taken: back to metric', shown(), METRIC);
check('and why', $('#mt-status').hasClass('error') && $('#mt-status').text() === 'Unknown option',
      $('#mt-status').text());

/* An answer asked before a change made here says the radio's old units: the change stays */
p.eval('update(false)');
$('#mt-units').val('1').trigger('change');
posted().ok({status: 'OK'});
answer(false);
same('an answer from before the change: US stays', shown(), US);
p.eval('update(false)');
answer(false);
same('the next answer: the radio in metric again (set on its screen)', shown(), METRIC);

if (failed) {
  console.log(failed + ' failed');
  process.exit(1);
}
console.log('PAGE OK: ' + n + ' cases, the page in both units, Units set from the page');
