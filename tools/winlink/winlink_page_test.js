#!/usr/bin/env node
/* The Winlink page's own script (webserver/.../views/winlink.html) under Node, on the small DOM of the audit's
 * browser-dom.js: the Before you connect card from the radio file's values as /api/winlink gives them. The Power
 * row: PWR under Winlink's 7 W, over it, the X6200 on its battery, where it is 5 W and the row says why, and a
 * transverter's TX power limit (#104), 0.5 W, said as such. The SWR hold row: none, on the radio's band (red), on
 * another band (the radio transmits), and from a GUI before #147 (every band held). The Filter row for XARA's
 * three bandwidths, the Connections card's Modem row with XARA's speed, and the Station card's rows for the modem
 * chosen (XARA's bandwidth and the VARA HF licence tick box with XARA only).
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

function swr(radio) {
  p.ctx.st = {state: 'running', modem_choice: 'ardop', bandwidth: 500, radio: Object.assign({}, RADIO, radio)};
  p.eval('renderRadio(st)');
  const red = p.$('#r-swr').css('color') === 'var(--err)';
  return p.$('#r-swr').text() + ', ' + p.$('#r-swr-n').text() + (red ? ' (red)' : '');
}

row = swr({});
check('no SWR hold', row === 'None, 5 minutes without TX on a band after high SWR stops a Winlink transmission there',
      row);
row = swr({swr_hold: 230, swr_hold_where: 'on 30m', swr_hold_here: 1});
check('the hold on 30 m, the radio there: red', row === '4 min left on 30m, High SWR stopped a Winlink transmission ' +
      'on 30m: no TX there until the hold ends. Check the antenna and the tuner (red)', row);
row = swr({swr_hold: 230, swr_hold_where: 'on 30m', swr_hold_here: 0});
check('the hold on 30 m, the radio on 40 m: it transmits', row === '4 min left on 30m, High SWR stopped a Winlink ' +
      'transmission on 30m. The radio is outside that hold: it transmits here', row);
row = swr({swr_hold: 230, swr_hold_where: 'near 7.250.000 MHz', swr_hold_here: 1});
check('a hold off the bands, by its frequency', row === '4 min left near 7.250.000 MHz, High SWR stopped a Winlink ' +
      'transmission near 7.250.000 MHz: no TX there until the hold ends. Check the antenna and the tuner (red)', row);
row = swr({swr_hold: 230});
check('a GUI before #147: the hold everywhere', row === '4 min left, High SWR stopped a Winlink transmission: no TX ' +
      'until the hold ends. Check the antenna and the tuner (red)', row);

function filter(st, radio) {
  p.ctx.st = Object.assign({state: 'running', modem_choice: 'xara', bandwidth: 500, mercury_bandwidth: 2300,
                            radio: Object.assign({}, RADIO, radio)}, st);
  p.eval('renderRadio(st)');
  const red = p.$('#r-filter').css('color') === 'var(--err)';
  return p.$('#r-filter').text() + ', ' + p.$('#r-filter-n').text() + (red ? ' (red)' : '');
}
row = filter({xara_bandwidth: 2300});
check("XARA at 2300 Hz: VARA HF's 350-2650 Hz, the RX filter covers it",
      row === "100-2900 Hz, Covers XARA's 350-2650 Hz at 2300 Hz", row);
row = filter({xara_bandwidth: 500});
check('XARA at 500 Hz: 1240-1760 Hz', row === "100-2900 Hz, Covers XARA's 1240-1760 Hz at 500 Hz", row);
row = filter({xara_bandwidth: 2750}, {tx_filter_low: 160, tx_filter_high: 3000});
check('XARA at 2750 Hz: 120-2880 Hz, past a TX filter from 160 Hz (red)', row === "100-2900 Hz, Does not cover " +
      "XARA's 120-2880 Hz at 2750 Hz; TX filter 160-3000 Hz: widen the TX filter (red)", row);
row = filter({modem_choice: 'mercury'});
check("Mercury's span as before", row === "100-2900 Hz, Covers Mercury's 450-2550 Hz at 2300 Hz", row);

p.eval(between(p.script, 'const PW_STATE', '// 14105000 as "14.105.000"'));     // the names the rows say
p.eval(between(p.script, '// The outcomes the supervisor keeps apart', 'const form = EditFlow.form'));
function modemRow(st) {
  p.ctx.st = Object.assign({state: 'running', service: 'running', session: 'idle', modem: 'ready'}, st);
  p.eval('renderConnections(st)');
  return p.$('#c-modem').text() + ', ' + p.$('#c-modem-n').text();
}
row = modemRow({modem_choice: 'xara'});
check("XARA ready, its REGISTERED line not read yet", row === "Ready, XARA on the radio's audio", row);
row = modemRow({modem_choice: 'xara', speed: 'free'});
check("XARA at the free speed", row === "Ready, VARA's free speed levels, XARA on the radio's audio", row);
row = modemRow({modem_choice: 'xara', speed: 'full'});
check('XARA at full speed, the licence ticked',
      row === "Ready, full speed (VARA HF licence ticked), XARA on the radio's audio", row);
row = modemRow({modem_choice: 'xara', modem: 'waiting', speed: 'free'});
check('waiting for the audio: the speed too', row === "Waiting for the radio's audio, VARA's free speed levels, " +
      "XARA on the radio's audio", row);
row = modemRow({modem_choice: 'mercury', speed: 'free'});
check('Mercury: no speed of its own', row === "Ready, Mercury on the radio's audio", row);
row = modemRow({state: 'off', modem_choice: 'xara', speed: 'full'});
check('off: Off', row === "Off, XARA on the radio's audio", row);

p.eval(between(p.script, "// The chosen modem's rows only", '$("#modem").on("change", showModem);'));
const ROWS = ['#row-bandwidth', '#row-mercury-bandwidth', '#row-xara-bandwidth', '#row-vara-licence'];
function shown(modem) {
  p.$('#modem').val(modem);
  p.eval('showModem()');
  return ROWS.filter(r => p.$(r)[0].style.display !== 'none').join(' ');
}
row = shown('ardop');
check("ARDOP: its bandwidth only", row === '#row-bandwidth', row);
row = shown('mercury');
check("Mercury: its bandwidth only", row === '#row-mercury-bandwidth', row);
row = shown('xara');
check("XARA: its bandwidth and the VARA HF licence", row === '#row-xara-bandwidth #row-vara-licence', row);

if (failed) process.exit(1);
console.log('PAGE OK');
