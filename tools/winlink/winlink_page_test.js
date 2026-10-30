#!/usr/bin/env node
/* The Winlink page's own script (webserver/.../views/winlink.html) under Node, on the small DOM of the audit's
 * browser-dom.js: the Before you connect card from the radio file's values as /api/winlink gives them. The Power
 * row: PWR under Winlink's 7 W, over it, the X6200 on its battery, where it is 5 W and the row says why, and a
 * transverter's TX power limit (#104), 0.5 W, said as such.
 * Prints PAGE OK; exits 1 at the first failure. Run from winlink_web_test.py, or `node winlink_page_test.js` */
'use strict';
const {page, between} = require('../../docs/audit/repro-2026-10-24/browser-dom.js');

let failed = false;
function check(what, ok, got) {
  console.log((ok ? '  ok: ' : 'FAIL: ') + what + (got !== undefined ? ' (' + got + ')' : ''));
  if (!ok) failed = true;
}

const p = page('winlink.html');
p.eval(between(p.script, '// 14105000 as "14.105.000"', '// The outcomes the supervisor keeps apart'));

/* The radio file of a GUI with a modem on its socket, simplex on 14.105 in USB-D, its filters wide */
const RADIO = {modem: 'connected', rx: 14105000, tx: 14105000, mode: 'USB-D', simplex: '', filter_low: 100,
               filter_high: 2900, tx_filter_low: 0, tx_filter_high: 0, pwr: 5.0, cap: 7.0, cap_battery: 0, app: '',
               locked: 0, swr_hold: 0, refused: '', refused_age: null};
function power(radio) {
  p.ctx.st = {state: 'running', modem_choice: 'ardop', bandwidth: 500, radio: Object.assign({}, RADIO, radio)};
  p.eval('renderRadio(st)');
  return p.$('#r-pwr').text() + ', ' + p.$('#r-pwr-n').text();
}

const UNDER = "The radio's PWR; Winlink at most ", BATTERY = ', as the X6200 runs on its battery';
const XVTR = ", the transverter's TX power limit";
let row = power({});
check('PWR 5 W: 5.0 W, Winlink at most 7 W', row === '5.0 W, ' + UNDER + '7 W', row);
row = power({pwr: 10.0});
check('PWR 10 W: 7.0 W, and that it is Winlink\'s most', row === '7.0 W, PWR is 10.0 W: Winlink sends at most 7 W', row);
row = power({cap: 5.0, cap_battery: 1});
check('the X6200 on its battery: 5.0 W, and why', row === '5.0 W, ' + UNDER + '5 W' + BATTERY, row);
row = power({pwr: 3.0, cap: 5.0, cap_battery: 1});
check('on its battery at PWR 3 W: 3.0 W, and why at most 5', row === '3.0 W, ' + UNDER + '5 W' + BATTERY, row);
row = power({pwr: 8.0, cap: 5.0, cap_battery: 1});
check('on its battery, PWR not down yet: 5.0 W', row === '5.0 W, PWR is 8.0 W: Winlink sends at most 5 W' + BATTERY, row);
row = power({cap_battery: undefined});
check('a GUI that writes no cap_battery: as before', row === '5.0 W, ' + UNDER + '7 W', row);
row = power({pwr: 0.5, cap: 0.5, cap_transverter: 1});
check('a transverter\'s 0.5 W limit: 0.5 W, at most 0.5 W, and why', row === '0.5 W, ' + UNDER + '0.5 W' + XVTR, row);
row = power({pwr: 2.0, cap: 0.5, cap_transverter: 1});
check('on the transverter, PWR not down yet: 0.5 W', row === '0.5 W, PWR is 2.0 W: Winlink sends at most 0.5 W' + XVTR, row);

if (failed) process.exit(1);
console.log('PAGE OK');
