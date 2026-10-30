#!/usr/bin/env node
/* Pat's web mailbox with patch 0005, in jsdom: what a Winlink message, another station or the network puts on
 * the page is shown as text and never becomes markup. Pat's own pages and scripts (web/dist of a patched Pat
 * source) against a stand-in for Pat's server, which answers with names, addresses and lines such as
 * <img src=x onerror=...>: the message list, the message window (its headers and attachments, one that is not
 * a picture saved rather than opened), the log pane, the status text, the prompt for the messages to
 * download, an error shown in the status bar and in the composer, the connect dialog's aliases, the gateway
 * list's prediction and the forms' catalog. Also that the mailbox and the configuration page run without
 * inline script, as their Content-Security-Policy wants (static/page-handlers.js does what their inline
 * handlers did).
 *
 *     git clone -q -b v1.0.0 https://github.com/la5nta/pat /tmp/pat && git -C /tmp/pat apply tools/pat/patches/*.patch
 *     NODE_PATH=<a node_modules with jsdom> node tools/pat/pat_ui_test.js /tmp/pat
 *
 * Prints each check and "N of N checks as they should be"; exit status 1 when one is not. Against Pat without
 * patch 0005 most of them fail. DEBUG=1 prints the page's console and errors and the requests. */
'use strict';
const fs = require('fs');
const http = require('http');
const path = require('path');
const {JSDOM, ResourceLoader, VirtualConsole} = require('jsdom');

const SRC = process.argv[2];
if (!SRC || !fs.existsSync(path.join(SRC, 'web/dist/index.html'))) {
  console.error('usage: node pat_ui_test.js <patched Pat source>');
  process.exit(2);
}
const DIST = path.join(SRC, 'web/dist');

let fails = 0, cases = 0;
function check(what, ok, got) {
  cases++;
  if (!ok) fails++;
  console.log((ok ? 'ok    ' : 'WRONG ') + what + (!ok && got !== undefined ? ': ' + JSON.stringify(got) : ''));
}
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function until(cond, ms = 15000) {   /* jsdom's requests take their time */
  for (const end = Date.now() + ms; !cond() && Date.now() < end;)
    await sleep(50);
}

/* What would run if it were taken for markup; jsdom loads no images, so the checks look at the DOM itself */
const IMG = '<img src=x onerror="window.ran=1">';
const QUOTE = 'x" onmouseover="window.ran=1" y="';
const MID = 'TESTMID00001';
const FILES = [
  {Name: IMG + '.txt', Size: 12},
  {Name: 'photo' + QUOTE + '.png', Size: 2048},
  {Name: 'a b#c?d.txt', Size: 3},
  {Name: 'RMS_Express_Form_ICS213-1.xml', Size: 99},
];
const LIST = [{MID: MID, Date: '2026-10-30T10:00:00Z' + IMG, From: {Addr: 'N0CALL' + IMG}, To: [{Addr: 'ME'}],
               Subject: 'Hello ' + IMG, Files: FILES, P2POnly: false, Unread: true}];
const MESSAGE = {MID: MID, Date: '2026-10-30T10:00:00Z', From: {Addr: 'N0CALL' + IMG}, To: [{Addr: 'TO' + IMG}],
                 Cc: [{Addr: 'CC' + IMG}], Subject: 'Hello ' + IMG, Body: 'hi', BodyHTML: '<p>hi</p>', Files: FILES,
                 P2POnly: false, Unread: true};
const RMSLIST = [{callsign: 'GW' + IMG, distance: 10, modes: 'ARDOP 2000', dial: {desc: '14.105 MHz'},
                  url: 'ardop:///GW?freq=14105', prediction: {link_quality: 50, output_values: {['k' + IMG]: 'v' + IMG},
                                                             output_raw: 'raw ' + IMG}}];
const CATALOG = {path: '/forms', version: '1.0' + IMG, forms: [],
                 folders: [{name: 'Folder ' + IMG, form_count: 1, folders: [], forms: [{name: 'Form ' + IMG, template_path: 'f.txt'}]}]};

/* The stand-in: Pat's page and files from web/dist, and the API's answers */
const posted = [];
const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://x');
  if (process.env.DEBUG)
    console.log('server:', req.method, req.url);
  const json = (v, code = 200) => { res.writeHead(code, {'Content-Type': 'application/json'}); res.end(JSON.stringify(v)); };
  const text = (v, code) => { res.writeHead(code, {'Content-Type': 'text/plain'}); res.end(v); };
  if (url.pathname === '/ui' || url.pathname === '/ui/config') {
    const page = fs.readFileSync(path.join(DIST, url.pathname === '/ui' ? 'index.html' : 'config.html'), 'utf8')
      .replace(/\{\{\.AppName\}\}/g, 'Pat').replace(/\{\{\.Version\}\}/g, 'v1.0.0').replace(/\{\{\.Mycall\}\}/g, 'N0CALL');
    res.writeHead(200, {'Content-Type': 'text/html'});
    return res.end(page);
  }
  if (url.pathname.startsWith('/dist/')) {
    const file = path.join(DIST, path.normalize(url.pathname.slice(6)));
    if (!file.startsWith(DIST) || !fs.existsSync(file)) return text('not found', 404);
    res.writeHead(200, {'Content-Type': file.endsWith('.js') ? 'application/javascript' : 'application/octet-stream'});
    return res.end(fs.readFileSync(file));
  }
  if (req.method === 'POST') {   /* each refused, with markup in the answer */
    req.resume();
    req.on('end', () => {
      posted.push(url.pathname);
      text('refused: ' + IMG, url.pathname.endsWith('/read') ? 500 : 400);
    });
    return;
  }
  switch (url.pathname) {
  case '/api/mailbox/in': return json(LIST);
  case '/api/mailbox/in/' + MID: return json(MESSAGE);
  case '/api/config/connect_aliases': return json({['alias' + IMG]: 'telnet://example'});
  case '/api/config': return json({});
  case '/api/bandwidths': return json({mode: '', bandwidths: []});
  case '/api/rmslist': return json(RMSLIST);
  case '/api/formcatalog': return json(CATALOG);
  case '/api/new-release-check': res.writeHead(204); return res.end();
  default: return text('not found', 404);
  }
});

/* The page's socket to Pat: one the test writes the server's messages into */
const sockets = [];
function fakeSocket(w) {
  w.WebSocket = class {
    constructor(url) { this.url = url; sockets.push(this); setTimeout(() => this.onopen && this.onopen(), 0); }
    send() {}
    close() {}
  };
}

/* The bundle keeps its jQuery to itself: the test clicks and submits as a browser would */
const click = (w, el) => el.dispatchEvent(new w.MouseEvent('click', {bubbles: true, cancelable: true}));
const submit = (w, el) => el.dispatchEvent(new w.Event('submit', {bubbles: true, cancelable: true}));

/* Elements under root with an on... attribute (an event handler written into the markup) */
function handlers(root) {
  return [...root.querySelectorAll('*')].filter(e => [...e.attributes].some(a => /^on/i.test(a.name)))
    .map(e => e.outerHTML.slice(0, 80));
}

/* A page of the stand-in's in jsdom, its scripts run */
function open(url) {
  const vc = new VirtualConsole();
  vc.on('jsdomError', e => { if (process.env.DEBUG || !/Not implemented/.test(e.message)) console.log('page error: ' + e.message); });
  if (process.env.DEBUG)
    vc.on('error', (...a) => console.log('page console:', ...a)).on('log', (...a) => console.log('page console:', ...a));
  return JSDOM.fromURL(url, {runScripts: 'dangerously', resources: new ResourceLoader(), pretendToBeVisual: true,
                             virtualConsole: vc, beforeParse: fakeSocket});
}

/* Pat's page sends what its inline handlers did to static/page-handlers.js, for the pages' policy (no inline script) */
function inlineScript(doc) {
  return handlers(doc.documentElement).concat([...doc.querySelectorAll('script:not([src])')].map(e => e.outerHTML.slice(0, 80)));
}

(async () => {
  await new Promise(r => server.listen(0, '127.0.0.1', r));
  const base = `http://127.0.0.1:${server.address().port}`;
  const dom = await open(base + '/ui');
  const w = dom.window, doc = w.document;
  const $ = s => doc.querySelector(s);
  await until(() => $('#folder tbody tr') && $('#aliasSelect option:not([value=""])') && $('#formFolderRoot button'));
  const ws = sockets[sockets.length - 1];
  const send = m => ws.onmessage({data: JSON.stringify(m)});
  const pageAsLoaded = inlineScript(doc);

  console.log('The message list');
  const row = $('#folder tbody tr');
  check('the row is there', !!row);
  const cells = row ? [...row.children].map(td => td.textContent) : [];
  check('the subject as text', cells[1] === 'Hello ' + IMG, cells[1]);
  check('the sender as text', cells[2] === 'N0CALL' + IMG, cells[2]);
  check('the date as text', cells[3] === LIST[0].Date, cells[3]);
  check('no markup from the message in the list', !$('#folder img') && handlers($('#folder')).length === 0,
        handlers($('#folder')));

  console.log('The message window');
  click(w, row);
  await until(() => $('#attachments .filesize'));
  const headers = $('#headers');
  check('the headers as text', headers.textContent === `Date: ${MESSAGE.Date}From: N0CALL${IMG}To: TO${IMG}Cc: CC${IMG}`,
        headers.textContent);
  check('no markup from the headers', !headers.querySelector('img') && handlers(headers).length === 0, headers.innerHTML);
  const att = $('#attachments');
  const names = [...att.querySelectorAll('.filename')].map(e => e.textContent);
  check('an attachment named <img ...> shown as its name', names.includes(IMG + '.txt'), names);
  check('the other one named with a space, # and ? too', names.includes('a b#c?d.txt'), names);
  const imgs = [...att.querySelectorAll('img')];
  check('one <img> in the attachments: the picture', imgs.length === 1, imgs.map(i => i.outerHTML));
  check('the picture\'s alt is its whole name', imgs[0] && imgs[0].getAttribute('alt') === 'photo' + QUOTE + '.png',
        imgs[0] && imgs[0].getAttribute('alt'));
  check('the picture\'s address is its name, encoded',
        imgs[0] && imgs[0].getAttribute('src') === `/api/mailbox/in/${MID}/` + encodeURIComponent('photo' + QUOTE + '.png'),
        imgs[0] && imgs[0].getAttribute('src'));
  const links = [...att.querySelectorAll('a')].map(a => a.getAttribute('href'));
  check('the links to the attachments encoded',
        links.includes(`/api/mailbox/in/${MID}/` + encodeURIComponent(IMG + '.txt')) &&
        links.includes(`/api/mailbox/in/${MID}/a%20b%23c%3Fd.txt`), links);
  const saved = [...att.querySelectorAll('a.attachment-preview')].filter(a => a.querySelector('.filename'));
  check('an attachment that is not a picture is saved under its name, not opened in a tab',
        saved.length === 2 && saved.every(a => a.getAttribute('download') === a.querySelector('.filename').textContent &&
                                               !a.hasAttribute('target')), saved.map(a => a.outerHTML.slice(0, 120)));
  const shown = imgs[0] && imgs[0].closest('a');
  check('the picture opens in a tab', shown && shown.getAttribute('target') === '_blank' && !shown.hasAttribute('download'),
        shown && shown.outerHTML.slice(0, 120));
  const form = [...att.querySelectorAll('a.btn')].map(a => a.textContent.trim());
  check('the form\'s button', form.length === 1 && form[0] === 'ICS213', form);
  check('no markup from the attachments\' names', handlers(att).length === 0, handlers(att));

  console.log('The log pane, the status text, the prompt and the progress bar');
  send({LogLine: 'RMS: ' + IMG});
  const log = $('#console');
  check('a log line as text', log.textContent === 'RMS: ' + IMG + '\n', log.textContent);
  check('no markup from the log', !log.querySelector('img'), log.innerHTML);
  send({Status: {connected: true, remote_addr: 'ardop:///GW' + IMG, active_listeners: [], http_clients: [], config_hash: 'a'}});
  check('the remote\'s address as text', $('#status_text').textContent === 'Connected ardop:///GW' + IMG,
        $('#status_text').textContent);
  check('no markup from the remote\'s address', !$('#status_text img'), $('#status_text').innerHTML);
  send({Status: {connected: false, remote_addr: '', active_listeners: ['ardop' + IMG], http_clients: [], config_hash: 'a'}});
  check('the listeners as text', $('#status_text').textContent === 'Listening ardop' + IMG, $('#status_text').textContent);
  send({Prompt: {id: 'p1', kind: 'multi-select', message: 'Select messages for download',
                 options: [{value: 'MID1', desc: 'N0CALL (100 bytes): ' + IMG, checked: true}]}});
  const label = $('#promptModal .checkbox-item');
  check('a message offered for download as text', label && label.textContent === ` N0CALL (100 bytes): ${IMG} (MID1)`,
        label && label.textContent);
  check('no markup from the offer', label && !label.querySelector('img'), label && label.innerHTML);
  send({Progress: {receiving: true, mid: MID, bytes_total: 10, bytes_transferred: 5, subject: 'a & b', done: false}});
  const progress = $('#navbar_progress .progress-text').textContent;
  check('the progress bar\'s subject as it is', progress === `Receiving ${MID} (10 bytes) - a & b`, progress);

  console.log('Errors from the server');
  /* the message is marked read after 2 s: the stand-in refuses it */
  await until(() => posted.some(p => p.endsWith('/read')) && $('#navbar_status').textContent);
  const status = $('#navbar_status');
  check('the refusal in the status bar as text', status.textContent === `Internal Server Error: refused: ${IMG}`,
        status.textContent);
  check('no markup from the refusal', !status.querySelector('img'), status.innerHTML);
  submit(w, $('#composer_form'));
  await until(() => $('#composer_error').textContent.startsWith('refused'));
  check('the composer\'s error as text', $('#composer_error').textContent === 'refused: ' + IMG, $('#composer_error').textContent);
  check('no markup from the composer\'s error', !$('#composer_error img'), $('#composer_error').innerHTML);

  console.log('The connect dialog, the gateway list and the forms');
  const options = [...doc.querySelectorAll('#aliasSelect option')].map(o => o.textContent);
  check('an alias as text', options.includes('alias' + IMG), options);
  click(w, $('[data-target="#rmslist-container"]'));
  await until(() => $('#rmslist tbody .link-quality-cell span'));
  const quality = $('#rmslist tbody .link-quality-cell span');
  check('the gateway list\'s row', !!quality);
  const pop = quality ? quality.getAttribute('data-content') : '';
  check('the prediction\'s pop-up escaped', pop.includes('k&lt;img') && !pop.includes('<img'), pop);
  if (quality)
    click(w, quality);
  await sleep(200);
  const modal = $('#rawDataModal');
  check('the prediction window\'s title as text', modal && modal.querySelector('.modal-title').textContent ===
        'Propagation Prediction Details: GW' + IMG, modal && modal.querySelector('.modal-title').textContent);
  check('the prediction as text', modal && modal.querySelector('pre').textContent === 'raw ' + IMG,
        modal && modal.querySelector('pre').textContent);
  check('no markup from the gateway list', modal && !modal.querySelector('img'), modal && modal.innerHTML.slice(0, 300));
  check('the forms\' version as text', $('#formsVersion a').textContent === '1.0' + IMG && $('#updateFormsVersion').textContent === '1.0' + IMG,
        [$('#formsVersion').innerHTML, $('#updateFormsVersion').innerHTML]);
  const folder = $('#formFolderRoot .folder-toggle'), formButton = $('#formFolderRoot .form-item button');
  check('a forms folder\'s name as text', folder && folder.textContent === 'Folder ' + IMG, folder && folder.textContent);
  check('a form\'s name as text', formButton && formButton.textContent === 'Form ' + IMG, formButton && formButton.textContent);
  check('no markup from the forms', !$('#formFolderRoot img') && !$('#formsVersion img'), $('#formFolderRoot').innerHTML.slice(0, 300));

  console.log('Without inline script (the pages\' Content-Security-Policy)');
  check('the mailbox page has no inline handler or script', pageAsLoaded.length === 0, pageAsLoaded);
  check('page-handlers.js is loaded before app.js',
        [...doc.querySelectorAll('script[src]')].map(e => e.getAttribute('src')).join() === 'dist/static/page-handlers.js,dist/js/app.js',
        [...doc.querySelectorAll('script[src]')].map(e => e.getAttribute('src')));
  const promptForm = $('#promptModal form');
  const enter = new w.Event('submit', {bubbles: true, cancelable: true});
  promptForm.dispatchEvent(enter);
  check('Enter in a prompt does not submit its form (the page stays)', promptForm.hasAttribute('data-no-submit') && enter.defaultPrevented);
  Object.defineProperty($('#composer'), 'getClientRects', {value: () => [{}]}); /* jsdom lays nothing out */
  send({Status: {connected: false, remote_addr: '', active_listeners: [], http_clients: [], config_hash: 'b'}});
  const reload = $('#navbar_status a');
  check('"Configuration has changed": a reload link without inline script',
        reload && reload.hasAttribute('data-reload') && !reload.hasAttribute('onclick'), reload && reload.outerHTML);
  const clicked = new w.MouseEvent('click', {bubbles: true, cancelable: true});
  if (reload)
    reload.dispatchEvent(clicked);
  check('the reload link handled by page-handlers.js', clicked.defaultPrevented);
  check('nothing ran', w.ran === undefined, w.ran);
  w.close();

  const config = (await open(base + '/ui/config')).window;
  await until(() => config.document.readyState === 'complete');   /* its scripts run */
  check('the configuration page has no inline handler or script', inlineScript(config.document).length === 0,
        inlineScript(config.document));
  const call = config.document.querySelector('#mycall');
  call.value = 'on6ure';
  call.dispatchEvent(new config.Event('input', {bubbles: true}));
  check('the callsign is upper case as it is typed', call.value === 'ON6URE', call.value);
  const revert = [...config.document.querySelectorAll('button')].find(b => b.textContent.trim() === 'Revert');
  check('Revert reloads without inline script', revert && revert.hasAttribute('data-reload') && !revert.hasAttribute('onclick'),
        revert && revert.outerHTML);
  config.close();

  console.log(`${cases - fails} of ${cases} checks as they should be`);
  server.close();
  process.exit(fails ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });
