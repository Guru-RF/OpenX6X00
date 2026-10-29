/* Issue #58 (Tailscale Funnel), the pages' side: radio_session.js on the Panel's own markup, the radio a stand-in
 * that answers what the page sends. Pairing paused: the pairing dialog says that no code shows and how long the
 * radio asks to wait, not "a code is on the radio's screen". The away link: its "#t=KEY" stored for the page's
 * address and taken out of the address bar. The pages' sockets on the Funnel page: wss on its own address. */
'use strict';
const {page, between} = require('./browser-dom.js');

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

/* ---- the away link: #t=KEY on the Funnel page ---- */
function funnelPage(view, hash) {
  const p = page(view);
  const stored = {}, replaced = [];
  p.ctx.localStorage = {getItem: k => (k in stored ? stored[k] : null), setItem: (k, v) => { stored[k] = String(v); },
                        removeItem: k => { delete stored[k]; }};
  p.ctx.location = {protocol: 'https:', host: 'x6200.tail1234.ts.net', hostname: 'x6200.tail1234.ts.net', hash: hash,
                    pathname: '/' + view.replace('.html', ''), search: ''};
  p.ctx.history = {replaceState: (s, t, url) => { replaced.push(url); p.ctx.location.hash = ''; }};
  p.run('static/js/radio_session.js');
  return {p, stored, replaced};
}
const KEY = '0123456789abcdef0123456789abcdef';
for (const view of ['panel.html', 'remote.html']) {
  const {p, stored, replaced} = funnelPage(view, '#t=' + KEY);
  const took = p.eval('RadioSession.takeLink()');
  check(view + ': the away link\'s key stored for this address', took && stored.x6100session === KEY, JSON.stringify(stored));
  check(view + ': and gone from the address bar', JSON.stringify(replaced) === JSON.stringify(['/' + view.replace('.html', '')]) && p.ctx.location.hash === '', JSON.stringify(replaced));
  check(view + ': RadioSession.token() gives it', p.eval('RadioSession.token()') === KEY);
  /* the page's own code: Funnel mode first, before its tabs read the address */
  const src = p.source;
  check(view + ': the key taken before anything reads the address', src.indexOf('if (FUNNEL) RadioSession.takeLink();') > 0 &&
        src.indexOf('if (FUNNEL) RadioSession.takeLink();') < Math.min(...['location.hash', '$.get('].map(s => src.indexOf(s)).filter(i => i > 0)));
  p.eval('var FUNNEL = true, secure = true, conf = null;' + between(src, '  function wsUrl(kind) {', '  $("#httpslink")'));
  check(view + ': its sockets on its own address', p.eval('wsUrl("remote")') === 'wss://x6200.tail1234.ts.net/ws/remote' &&
        p.eval('wsUrl("tci")') === 'wss://x6200.tail1234.ts.net/ws/tci', p.eval('wsUrl("remote")'));
  const on = between(src, 'onOpen: function() { greeted = false;', '},');
  check(view + ': the key first on a connection', on.indexOf('ses.onOpen()') >= 0 && on.indexOf('ses.onOpen()') < on.indexOf('send('), on.trim().slice(0, 90));
}
{
  const {p, stored, replaced} = funnelPage('panel.html', '#t=<img src=x>');
  const took = p.eval('RadioSession.takeLink()');
  check('a link with something else than a key: nothing stored, gone from the address bar', !took && !('x6100session' in stored) && replaced.length === 1);
  const q = funnelPage('panel.html', '#ft8');
  check('a tab in the address (#ft8): left as it is', !q.p.eval('RadioSession.takeLink()') && q.replaced.length === 0 && q.p.ctx.location.hash === '#ft8');
}

const bad = results.filter(r => !r.ok).length;
console.log(bad ? bad + ' funnel page case(s) WRONG' : 'All ' + results.length + ' funnel page cases as they should be (#58)');
process.exitCode = bad ? 1 : 0;
