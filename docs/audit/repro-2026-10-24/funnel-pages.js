/* Issue #58 (Tailscale Funnel), the pages' side: radio_session.js on the Panel's own markup, the radio a stand-in
 * that answers what the page sends. Pairing paused: the pairing dialog says that no code shows and how long the
 * radio asks to wait, not "a code is on the radio's screen". The away link: its "#t=KEY" stored for the page's
 * address and taken out of the address bar. The pages' sockets on the Funnel page: wss on its own address; on the
 * radio's own https page too, through its https server (#58: on 50003 and 50004 Firefox wanted the certificate
 * accepted again). */
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
/* ---- the radio's own pages: an https page's sockets on its own address and port as well, one certificate; an http
 * page's on the GUI's ports, as before ---- */
for (const view of ['panel.html', 'remote.html']) {
  const p = page(view);
  p.eval('var FUNNEL = false, secure = true, conf = {port: 50002, tci_port: 50001, wss_port: 50003, wss_tci_port: 50004};' +
         between(p.source, '  function wsUrl(kind) {', '  $("#httpslink")'));
  const urls = (protocol, host, hostname, isSecure) => {
    p.ctx.location = {protocol, host, hostname, hash: '', pathname: '/' + view.replace('.html', ''), search: ''};
    p.eval('secure = ' + isSecure);
    return p.eval('wsUrl("remote")') + ' ' + p.eval('wsUrl("tci")');
  };
  for (const [what, protocol, host, hostname, isSecure, want] of [
    ['https, port 443', 'https:', 'x6100.local', 'x6100.local', true, 'wss://x6100.local/ws/remote wss://x6100.local/ws/tci'],
    ['https on a port of its own (a PC)', 'https:', '127.0.0.1:8443', '127.0.0.1', true,
     'wss://127.0.0.1:8443/ws/remote wss://127.0.0.1:8443/ws/tci'],
    ['https by an IPv6 address', 'https:', '[fe80::1]', '[fe80::1]', true, 'wss://[fe80::1]/ws/remote wss://[fe80::1]/ws/tci'],
    ['http: the GUI\'s own ports', 'http:', '192.168.1.50', '192.168.1.50', false,
     'ws://192.168.1.50:50002/ ws://192.168.1.50:50001/']]) {
    const got = urls(protocol, host, hostname, isSecure);
    check(view + ', ' + what, got === want, got);
  }
}

{
  const {p, stored, replaced} = funnelPage('panel.html', '#t=<img src=x>');
  const took = p.eval('RadioSession.takeLink()');
  check('a link with something else than a key: nothing stored, gone from the address bar', !took && !('x6100session' in stored) && replaced.length === 1);
  const q = funnelPage('panel.html', '#ft8');
  check('a tab in the address (#ft8): left as it is', !q.p.eval('RadioSession.takeLink()') && q.replaced.length === 0 && q.p.ctx.location.hash === '#ft8');
}

/* ---- Use away from home (the LAN Panel, Funnel on) ---- */
function awayPanel(url, role, name) {
  const p = page('panel.html');
  p.run('static/js/radio_session.js');
  p.eval(`var sent = [];
    var ses = RadioSession.create({send: function(s) { sent.push(s); return true; }, status: function() {}, el: document.getElementById("ses"),
                                   awayUrl: function() { return ${JSON.stringify(url)}; }});
    ses.onText("role", ${JSON.stringify(role + ' 1 ' + name)});`);
  return p;
}
const awayButton = p => [...p.doc.querySelectorAll('#ses button')].find(b => b.textContent === 'Use away from home');
{
  const URL = 'https://x6200.tail1234.ts.net/panel';
  const p = awayPanel(URL, 'operate', 'Kitchen phone');
  const b = awayButton(p);
  check('Funnel on, a paired browser: Use away from home shown', b && !b.hasAttribute('hidden'));
  p.$(b).trigger('click');
  check('its dialog names the address on the internet', dialog(p).includes(URL), dialog(p).slice(0, 80));
  const make = [...p.doc.querySelectorAll('.spop button')].find(x => x.textContent === 'Make the key and open');
  p.$(make).trigger('click');
  check('Make the key asks the radio', JSON.stringify(p.ctx.sent) === '["away"]', JSON.stringify(p.ctx.sent));
  p.eval(`ses.onText("away", "${KEY}")`);
  const a = p.doc.querySelector('.spop a');
  check('the radio\'s key: a link to the Panel there, the key after #', a && a.getAttribute('href') === URL + '#t=' + KEY, a && a.getAttribute('href'));
  check('the link opens apart from this page', a && a.getAttribute('target') === '_blank' && /noopener/.test(a.getAttribute('rel')));
  check('the key is not in the dialog\'s text', !dialog(p).includes(KEY));
  check('not shown without Funnel', awayButton(awayPanel(null, 'operate', 'Kitchen phone')).hasAttribute('hidden'));
  check('not shown to a browser that only watches', awayButton(awayPanel(URL, 'view', '-')).hasAttribute('hidden'));
  check('not shown to an away key itself', awayButton(awayPanel(URL, 'operate', 'Kitchen phone (away)')).hasAttribute('hidden'));
  p.eval(`ses.sessionsOpen(); ses.onText("sessions", '[{"n":1,"name":"Kitchen phone (away)","role":"operate","tx":1,"last":0,"ip":"203.0.113.7","funnel":1,"online":1,"me":0}]')`);
  check('Sessions: an away key over Funnel, its address', dialog(p).includes('203.0.113.7 via Funnel'), dialog(p).slice(dialog(p).indexOf('Kitchen'), dialog(p).indexOf('Kitchen') + 60));
}
{
  /* the Panel's own: the away address only from the configurator at home (/api/remote's funnel_url), never on the Funnel page */
  const src = page('panel.html').source;
  check('the Panel passes the address at home only', /awayUrl: function\(\) \{ return !FUNNEL && conf && conf\.funnel_url/.test(src));
}

/* ---- Remote > Tailscale: the Funnel card ---- */
{
  const p = page('tailscale.html');
  p.run(p.script, 'views/tailscale.html');
  const answer = st => { const r = p.$.requests.shift(); r.ok(st); };
  const base = {state: 'running', text: 'Connected', name: 'x6200.tail1234.ts.net', ip: '100.101.102.103', enabled: true};
  answer(Object.assign({}, base, {funnel: 'off', funnel_wanted: false}));
  const sw = p.doc.getElementById('funnel');
  check('Tailscale page: Funnel off, its switch ready while Tailscale is connected', !sw.checked && !sw.disabled);
  p.$(sw).prop('checked', true).trigger('change');
  const post = p.$.requests.find(r => r.type === 'POST');
  check('the switch on: funnel_on to the radio', post && post.body.action === 'funnel_on', post && JSON.stringify(post.body));
  post.ok(Object.assign({}, base, {funnel: 'enable', funnel_wanted: true, funnel_url: 'https://login.tailscale.com/f/funnel?node=nTEST123',
                                   funnel_text: 'Turn on HTTPS and Funnel for your tailnet: open the link and allow it'}));
  const allow = p.doc.getElementById('allow');
  check('not allowed on the tailnet yet: the link to allow it', p.doc.getElementById('allowrow').style.display !== 'none' &&
        allow.getAttribute('href') === 'https://login.tailscale.com/f/funnel?node=nTEST123', allow.getAttribute('href'));
  p.$.requests.length = 0;
  p.eval('refresh()');
  answer(Object.assign({}, base, {funnel: 'on', funnel_wanted: true, public: 'https://x6200.tail1234.ts.net/panel', funnel_text: 'On: the Panel is on the internet'}));
  const pub = p.doc.querySelector('#pub a');
  check('on: the address on the internet', p.doc.getElementById('pubrow').style.display !== 'none' && pub && pub.getAttribute('href') === 'https://x6200.tail1234.ts.net/panel'
        && p.doc.getElementById('allowrow').style.display === 'none');
  check('on: its status', p.doc.getElementById('ftext').textContent === 'On: the Panel is on the internet');
  p.eval('refresh()');
  answer({state: 'login', text: 'Waiting for your login', enabled: true, funnel_wanted: false, funnel: 'off'});
  check('Tailscale not connected: the switch waits (off)', sw.disabled);
}

/* ---- the Update page: a step back to a version before Funnel, refused while Funnel cannot be taken off ---- */
{
  const WHY = 'error not gone back: Funnel could not be made sure to be off (tailscaled: serve-config: HTTP 500 netMap is nil). ' +
              'v0.3.6 knows nothing of Tailscale Funnel and would leave TCI and the browser remote on the internet without a key: ' +
              'turn Funnel off first, or Tailscale, wait until it says Off, and try again';
  const SAID = 'Failed: ' + WHY.slice(6);
  const updatePage = () => {
    const p = page('update.html');
    let ids = 0;
    p.ctx.setInterval = () => ++ids;                 /* the page keeps its poll's id, and tests it */
    p.ctx.$.post = (url, data, cb) => { const r = p.$.ajax({url: url, type: 'POST', success: cb}); r.fail = () => r; return r; };
    p.run(p.script, 'views/update.html');
    p.$.requests.shift().ok({running: '0.3.7', job: {running: false, what: '', result: ''}});
    return p;
  };
  const rstatus = p => p.doc.getElementById('rstatus');
  {
    const p = updatePage();
    p.$('#rollback').trigger('click');
    const post = p.$.requests.shift();
    check('Update page: Go back asks the radio', post && post.type === 'POST' && post.url === '/api/update/rollback');
    post.ok({running: '0.3.7', job: {running: false, what: 'rollback', result: WHY}});   /* refused before this answer */
    check('a step back refused before the answer came: the reason and what to do shown', rstatus(p).textContent === SAID &&
          rstatus(p).classList.contains('error'), rstatus(p).textContent.slice(0, 80));
  }
  {
    const p = updatePage();
    p.$('#rollback').trigger('click');
    p.$.requests.shift().ok({running: '0.3.7', job: {running: true, what: 'rollback', result: ''}});
    const working = rstatus(p).textContent;
    p.eval('refresh()');                               /* the poll, once the wait for Funnel ran out */
    p.$.requests.shift().ok({running: '0.3.7', job: {running: false, what: 'rollback', result: WHY}});
    check('refused after the wait: "Working..." first, then the reason', working === 'Working...' && rstatus(p).textContent === SAID,
          working + ' / ' + rstatus(p).textContent.slice(0, 60));
  }
  {
    /* The page's own look at the radio, still out when Go back is pressed, answers after the click with a job of
       before (an earlier try); then the click's own answer: the step back just refused, over before it came back */
    const p = page('update.html');
    p.ctx.setInterval = () => 1;
    p.ctx.$.post = (url, data, cb) => { const r = p.$.ajax({url: url, type: 'POST', success: cb}); r.fail = () => r; return r; };
    p.run(p.script, 'views/update.html');
    const look = p.$.requests.shift();
    p.$('#rollback').trigger('click');
    const post = p.$.requests.shift();
    look.ok({running: '0.3.7', job: {running: false, what: 'rollback', result: 'error nothing to roll back'}});
    post.ok({running: '0.3.7', job: {running: false, what: 'rollback', result: WHY}});
    check('an older answer coming in after the click: the step back just refused is what is shown',
          rstatus(p).textContent === SAID, rstatus(p).textContent.slice(0, 80));
  }
  {
    const p = page('update.html');
    p.run(p.script, 'views/update.html');
    p.$.requests.shift().ok({running: '0.3.6', job: {running: false, what: 'rollback', result: 'ok back to 0.3.6'}});
    check('the page opened later: a job of before not shown as if just done', rstatus(p).textContent === '', rstatus(p).textContent);
  }
}

const bad = results.filter(r => !r.ok).length;
console.log(bad ? bad + ' funnel page case(s) WRONG' : 'All ' + results.length + ' funnel page cases as they should be (#58)');
process.exitCode = bad ? 1 : 0;
