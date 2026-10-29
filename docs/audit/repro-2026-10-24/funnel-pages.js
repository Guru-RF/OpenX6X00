/* Issue #58 (Tailscale Funnel), the pages' side: radio_session.js on the Panel's own markup, the radio a stand-in
 * that answers what the page sends. Pairing paused: the pairing dialog says that no code shows and how long the
 * radio asks to wait, not "a code is on the radio's screen". */
'use strict';
const {page} = require('./browser-dom.js');

const results = [];
function check(what, ok, got) {
  results.push({what, ok});
  console.log((ok ? 'ok    ' : 'WRONG ') + what + (got !== undefined ? ': ' + got : ''));
}

function panel() {
  const p = page('panel.html');
  p.run('static/js/radio_session.js');
  p.eval(`var sent = [], said = [];
    var ses = RadioSession.create({send: function(s) { sent.push(s); return true; }, status: function(t) { said.push(t); },
                                   el: document.getElementById("ses")});`);
  return p;
}
const dialog = p => { const b = p.doc.querySelector('.spop .sbox'); return b ? b.textContent : ''; };

/* ---- pairing paused (the radio's limit on new codes) ---- */
{
  const p = panel();
  p.eval('ses.pairOpen()');
  check('Pair asks the radio for a code', JSON.stringify(p.ctx.sent) === '["pair?"]', JSON.stringify(p.ctx.sent));
  p.eval('ses.onText("err", "pairing paused: a new code in 25 s, or Settings > General > BROWSER REMOTE > Pair code on the radio")');
  const text = dialog(p);
  check('paused: the dialog says no code shows, and for how long', text.includes("No code on the radio's screen now: a new code in 25 s"), text.slice(text.indexOf('No code'), text.indexOf('No code') + 60));
  check('paused: the dialog asks nothing more of the radio', JSON.stringify(p.ctx.sent) === '["pair?"]', JSON.stringify(p.ctx.sent));
}

const bad = results.filter(r => !r.ok).length;
console.log(bad ? bad + ' funnel page case(s) WRONG' : 'All ' + results.length + ' funnel page cases as they should be (#58)');
process.exitCode = bad ? 1 : 0;
