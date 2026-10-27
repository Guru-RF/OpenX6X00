/* C12: a row's state after a save, as the configurator pages show it. edit_flow.js, list_editor.js and the
 * pages' own scripts (CW messages, Memories, Macros, Bands) run on the pages' markup; the radio's web
 * server is the harness, which answers each request when it chooses: late, out of order, with a failure. */
'use strict';
const {page} = require('./browser-dom.js');

function open(view, libs, answers) {
  const p = page(view);
  libs.forEach(f => p.run(f));
  p.run(p.script, 'views/' + view);
  for (const a of answers) next(p, a[0]).ok(a[1]);
  return p;
}
/* The request the page made for url (the oldest not answered) */
function next(p, url) {
  const r = p.$.requests.find(q => !q.done && q.url === url);
  if (!r) throw new Error('no request for ' + url + ' (' + p.$.requests.filter(q => !q.done).map(q => q.type + ' ' + q.url).join(', ') + ')');
  return r;
}
function view(p, tr) {
  const rst = tr.querySelector('.rst'), save = tr.querySelector('.save');
  return {values: [...tr.querySelectorAll('input, select')].map(i => i.type === 'checkbox' ? i.checked : i.value).join('|'),
          st: rst ? rst.getAttribute('data-st') : null, label: rst ? rst.textContent : '', save: save ? !save.disabled : null,
          warns: p.leaving(), bar: (p.doc.querySelector('.dp-tools span') || {textContent: ''}).textContent};
}
const results = [];
function check(what, got, want) {
  const ok = Object.keys(want).every(k => JSON.stringify(got[k]) === JSON.stringify(want[k]));
  results.push(ok);
  console.log((ok ? 'ok    ' : 'WRONG ') + what + ': ' + Object.keys(want).map(k => k + '=' + JSON.stringify(got[k])).join(' ') + (ok ? '' : '  (want ' + Object.keys(want).map(k => k + '=' + JSON.stringify(want[k])).join(' ') + ')'));
}
/* A case that cannot go on (a Save that sent nothing, a row that is not there) counts as wrong, and the next runs */
function scenario(fn) {
  try { fn(); } catch (e) { results.push(false); console.log('WRONG ... and the case could not go on: ' + e.message); }
}
const cwList = msgs => ({max: 64, messages: msgs.map((t, i) => ({id: i, text: t}))});

/* ---- CW messages ---- */
function cw(msgs) { return open('cw_messages.html', ['static/js/edit_flow.js'], [['/api/cw_messages', cwList(msgs)]]); }
const cwRow = (p, k) => p.doc.querySelector('#messages tbody tr[data-key="' + k + '"]');
scenario(() => {
  /* the audit's case: A on the radio, B sent, C typed while it is on its way, the answer says B */
  const p = cw(['CQ A', 'TU 73']);
  p.type(cwRow(p, 0).querySelector('input'), 'CQ B');
  cwRow(p, 0).querySelector('.save').click();
  const req = next(p, '/api/cw_messages/0');
  p.type(cwRow(p, 0).querySelector('input'), 'CQ C');
  check('CW, C typed while B is saved: during the save', view(p, cwRow(p, 0)), {values: 'CQ C', st: 'saving', save: false, warns: true});
  req.ok(cwList(['CQ B', 'TU 73']));
  check('CW, C typed while B is saved: B saved, C stays unsaved', view(p, cwRow(p, 0)), {values: 'CQ C', st: 'unsaved', save: true, warns: true, bar: '1 unsaved'});
  cwRow(p, 0).querySelector('.save').click();
  check('CW, then C saved', {sent: next(p, '/api/cw_messages/0').body}, {sent: {text: 'CQ C'}});
  next(p, '/api/cw_messages/0').ok(cwList(['CQ C', 'TU 73']));
  check('CW, then C saved: Saved', view(p, cwRow(p, 0)), {values: 'CQ C', st: 'saved', save: false, warns: false, bar: ''});
});
scenario(() => {
  /* a failed save keeps its draft: Save again without another edit, Save all, Discard, the warning */
  const p = cw(['CQ A', 'TU 73']);
  p.type(cwRow(p, 1).querySelector('input'), 'TU 5NN');
  cwRow(p, 1).querySelector('.save').click();
  next(p, '/api/cw_messages/1').fail(0, 'error');
  check('CW, a save the server never got', view(p, cwRow(p, 1)), {values: 'TU 5NN', st: 'failed', save: true, warns: true, bar: '1 unsaved'});
  cwRow(p, 1).querySelector('.save').click();
  const again = p.$.requests.filter(q => q.url === '/api/cw_messages/1');
  check('CW, Save again without another edit', {requests: again.length, sent: again[again.length - 1].body}, {requests: 2, sent: {text: 'TU 5NN'}});
  again[1].fail(500, 'error', {msg: 'Text too long'});
  check('CW, refused by the radio', view(p, cwRow(p, 1)), {st: 'failed', label: 'Failed: Text too long', save: true, warns: true});
  [...p.doc.querySelectorAll('.dp-tools button')].find(b => b.textContent === 'Discard').click();
  check('CW, Discard', view(p, cwRow(p, 1)), {values: 'TU 73', st: '', save: false, warns: false, bar: ''});
});
/* Save all: the threaded web server could answer two saves the other way round, the later answer with the
 * older list, so a table's requests go one at a time, each after the answer to the one before */
const onTheirWay = p => p.$.requests.filter(q => !q.done).map(q => q.type + ' ' + q.url);
scenario(() => {
  const p = cw(['CQ A', 'TU 73']);
  p.type(cwRow(p, 0).querySelector('input'), 'CQ A2');
  p.type(cwRow(p, 1).querySelector('input'), 'TU 2');
  [...p.doc.querySelectorAll('.dp-tools button')].find(b => b.textContent === 'Save all').click();
  check('CW, Save all: one request on its way', {requests: onTheirWay(p)}, {requests: ['PUT /api/cw_messages/0']});
  check('CW, Save all: the second row waits its turn', view(p, cwRow(p, 1)), {values: 'TU 2', st: 'saving', save: false, warns: true});
  cwRow(p, 1).querySelector('.save').click();
  check('CW, Save all: Save on a row waiting its turn sends nothing', {requests: onTheirWay(p)}, {requests: ['PUT /api/cw_messages/0']});
  next(p, '/api/cw_messages/0').ok(cwList(['CQ A2', 'TU 73']));
  check('CW, Save all, the first answer: the first row', view(p, cwRow(p, 0)), {values: 'CQ A2', st: 'saved'});
  check('CW, Save all, the first answer: the second row still saving, its text kept', view(p, cwRow(p, 1)), {values: 'TU 2', st: 'saving', save: false, warns: true});
  const r1 = next(p, '/api/cw_messages/1');
  check('CW, Save all: then the second request, with what the row held at Save all', {requests: onTheirWay(p), sent: r1.body}, {requests: ['PUT /api/cw_messages/1'], sent: {text: 'TU 2'}});
  r1.ok(cwList(['CQ A2', 'TU 2']));
  check('CW, Save all, the second answer: the second row', view(p, cwRow(p, 1)), {values: 'TU 2', st: 'saved', warns: false, bar: ''});
  check('CW, Save all, the second answer: the first row as the radio has it', view(p, cwRow(p, 0)), {values: 'CQ A2', label: '', save: false});
});
scenario(() => {
  /* a row's save on its way, another row's Save waits; the first times out, the second goes out and its
   * answer reads the list again: the first row still says Failed, its text kept */
  const p = cw(['CQ A', 'TU 73']);
  p.type(cwRow(p, 0).querySelector('input'), 'CQ A2');
  cwRow(p, 0).querySelector('.save').click();
  p.type(cwRow(p, 1).querySelector('input'), 'TU 2');
  cwRow(p, 1).querySelector('.save').click();
  check('CW, a second row\'s Save while the first is on its way: it waits', {requests: onTheirWay(p)}, {requests: ['PUT /api/cw_messages/0']});
  next(p, '/api/cw_messages/0').fail(0, 'timeout');
  const timeout = 'Failed: No reply from the radio in 15 s: reload the page to see what was saved';
  check('CW, the first save times out', view(p, cwRow(p, 0)), {values: 'CQ A2', st: 'failed', label: timeout, save: true, warns: true});
  next(p, '/api/cw_messages/1').ok(cwList(['CQ A', 'TU 2']));
  check('CW, then the second row\'s answer: it is saved', view(p, cwRow(p, 1)), {values: 'TU 2', st: 'saved'});
  check('CW, then the second row\'s answer: the first still failed, its text kept', view(p, cwRow(p, 0)), {values: 'CQ A2', st: 'failed', label: timeout, save: true, warns: true, bar: '1 unsaved'});
});
scenario(() => {
  /* Save all, the first answered, the second (found by its ticket in the table read again) times out */
  const p = cw(['CQ A', 'TU 73']);
  p.type(cwRow(p, 0).querySelector('input'), 'CQ A2');
  p.type(cwRow(p, 1).querySelector('input'), 'TU 2');
  [...p.doc.querySelectorAll('.dp-tools button')].find(b => b.textContent === 'Save all').click();
  next(p, '/api/cw_messages/0').ok(cwList(['CQ A2', 'TU 73']));
  next(p, '/api/cw_messages/1').fail(0, 'timeout');
  check('CW, Save all, the second save times out after the first answer', view(p, cwRow(p, 1)), {values: 'TU 2', st: 'failed', save: true, warns: true, bar: '1 unsaved'});
});
scenario(() => {
  /* two new messages, Save all: each added in turn, both stay on the page */
  const p = cw(['CQ A']);
  p.doc.getElementById('add').click();
  p.doc.getElementById('add').click();
  const news = [...p.doc.querySelectorAll('#messages tbody tr[data-key=""]')];
  p.type(news[0].querySelector('input'), 'N1');
  p.type(news[1].querySelector('input'), 'N2');
  [...p.doc.querySelectorAll('.dp-tools button')].find(b => b.textContent === 'Save all').click();
  check('CW, two new messages, Save all: one added at a time', {requests: onTheirWay(p)}, {requests: ['POST /api/cw_messages']});
  next(p, '/api/cw_messages').ok(cwList(['CQ A', 'N1']));
  check('CW, two new messages, the first added: the second waits, its text kept', view(p, news[1]), {values: 'N2', st: 'saving', warns: true});
  check('CW, then the second request', {sent: next(p, '/api/cw_messages').body}, {sent: {text: 'N2'}});
  next(p, '/api/cw_messages').ok(cwList(['CQ A', 'N1', 'N2']));
  const rows = [...p.doc.querySelectorAll('#messages tbody tr')].map(t => t.getAttribute('data-key') + ':' + t.querySelector('input').value + ':' + (t.querySelector('.rst').getAttribute('data-st') || ''));
  check('CW, two new messages, both added', {rows: rows, warns: p.leaving(), bar: view(p, cwRow(p, 0)).bar}, {rows: ['0:CQ A:', '1:N1:', '2:N2:saved'], warns: false, bar: ''});
});
scenario(() => {
  /* a delete while a save is on its way waits for its answer too */
  const p = cw(['CQ A', 'TU 73']);
  p.type(cwRow(p, 0).querySelector('input'), 'CQ A2');
  cwRow(p, 0).querySelector('.save').click();
  cwRow(p, 1).querySelector('.remove').click();
  check('CW, a delete while a save is on its way: it waits', {requests: onTheirWay(p)}, {requests: ['PUT /api/cw_messages/0']});
  next(p, '/api/cw_messages/0').ok(cwList(['CQ A2', 'TU 73']));
  check('CW, then the delete goes out', {requests: onTheirWay(p)}, {requests: ['DELETE /api/cw_messages/1']});
  next(p, '/api/cw_messages/1').ok(cwList(['CQ A2']));
  check('CW, then deleted, the saved row kept', Object.assign(view(p, cwRow(p, 0)), {rows: p.doc.querySelectorAll('#messages tbody tr').length}), {values: 'CQ A2', save: false, warns: false, rows: 1});
});
scenario(() => {
  /* a new message, typed on while it is added */
  const p = cw(['CQ A']);
  p.doc.getElementById('add').click();
  const tr = p.doc.querySelector('#messages tbody tr[data-key=""]');
  p.type(tr.querySelector('input'), 'NEW');
  tr.querySelector('.save').click();
  const req = next(p, '/api/cw_messages');
  p.type(tr.querySelector('input'), 'NEWER');
  req.ok(cwList(['CQ A', 'NEW']));
  check('CW, a new message typed on while it is added', view(p, cwRow(p, 1)), {values: 'NEWER', st: 'unsaved', save: true, warns: true});
  check('CW, the new row itself is gone', {newRows: p.doc.querySelectorAll('#messages tbody tr[data-key=""]').length}, {newRows: 0});
});

/* ---- Memories: a slot that times out, a channel renamed while its save is on its way ---- */
const slot = (id, f) => ({id: id, frequency: f, mode: 'USB', agc: 'AUTO', pre: false, att: false});
const chan = (n, f) => ({name: n, frequency: f, mode: 'USB-D', agc: 'FAST', pre: false, att: false});
function memories() {
  return open('memories.html', ['static/js/edit_flow.js'], [['/api/memories', {slots: [slot(1, 14074000), slot(2, null)]}], ['/api/channels', {max: 100, channels: [chan('FT8 20', 14074000)]}]]);
}
scenario(() => {
  const p = memories();
  const tr = () => p.doc.querySelector('#slots tbody tr[data-key="1"]');
  p.type(tr().querySelector('.freq'), '14.0745');
  tr().querySelector('.save').click();
  next(p, '/api/memories/1').fail(0, 'timeout');
  check('Memories, a slot\'s save times out', view(p, tr()), {st: 'failed', save: true, warns: true});
  tr().querySelector('.save').click();
  const req = next(p, '/api/memories/1');
  check('Memories, Save again', {sent: req.body.frequency}, {sent: 14074500});
  req.ok({slots: [slot(1, 14074500), slot(2, null)]});
  check('Memories, then saved', view(p, tr()), {values: '14.0745|USB|AUTO|false|false', st: 'saved', save: false, warns: false});
});
scenario(() => {
  const p = memories();
  const tr = k => p.doc.querySelector('#channels tbody tr[data-key="' + k + '"]');
  p.type(tr('FT8 20|14074000|USB-D').querySelector('.name'), 'FT8 X');
  tr('FT8 20|14074000|USB-D').querySelector('.save').click();
  const req = next(p, '/api/channels/0');
  p.type(tr('FT8 20|14074000|USB-D').querySelector('.name'), 'FT8 Y');
  req.ok({max: 100, channels: [chan('FT8 X', 14074000)]});
  check('Memories, a channel renamed X, typed Y while it is saved', view(p, tr('FT8 X|14074000|USB-D')), {values: 'FT8 Y|14.074|USB-D|FAST|false|false', st: 'unsaved', save: true, warns: true});
});

scenario(() => {
  /* the slots and the channels are lists of their own: a save of each goes out at once */
  const p = memories();
  const st = p.doc.querySelector('#slots tbody tr[data-key="1"]'), ch = p.doc.querySelector('#channels tbody tr[data-key="FT8 20|14074000|USB-D"]');
  p.type(st.querySelector('.freq'), '14.0745'); st.querySelector('.save').click();
  p.type(ch.querySelector('.name'), 'FT8 X'); ch.querySelector('.save').click();
  check('Memories, a slot and a channel saved together: both on their way', {requests: onTheirWay(p)}, {requests: ['PUT /api/memories/1', 'PUT /api/channels/0']});
});

/* ---- Macros ---- */
scenario(() => {
  const p = open('macros.html', ['static/js/edit_flow.js'], [['/api/macros/psk', {max: 8, name_max: 10, text_max: 60, macros: [{id: 0, name: 'CQ', text: 'CQ CQ de ON6URE'}]}],
                                                           ['/api/macros/olivia', {max: 8, name_max: 10, text_max: 60, macros: []}], ['/api/macros/jtty', {max: 8, name_max: 10, text_max: 60, macros: []}],
                                                           ['/api/js8_texts', {info: '', status: ''}]]);
  const tr = k => p.doc.querySelector('#t-psk tbody tr[data-key="' + k + '"]');
  const k0 = 'CQ\tCQ CQ de ON6URE';
  p.type(tr(k0).querySelectorAll('input')[1], 'CQ CQ CQ de ON6URE');
  tr(k0).querySelector('.save').click();
  const req = next(p, '/api/macros/psk/0');
  p.type(tr(k0).querySelectorAll('input')[1], 'CQ CQ CQ de ON6URE pse k');
  req.ok({max: 8, name_max: 10, text_max: 60, macros: [{id: 0, name: 'CQ', text: 'CQ CQ CQ de ON6URE'}]});
  check('Macros, typed on while saved', view(p, tr('CQ\tCQ CQ CQ de ON6URE')), {values: 'CQ|CQ CQ CQ de ON6URE pse k', st: 'unsaved', save: true, warns: true});
});

/* ---- Bands (list_editor.js): one row edited at a time, read again after a save ---- */
const band = (id, name) => ({id: id, name: name, start_freq: 14000000, stop_freq: 14350000, params: {vfoa_mode: 2, tx_shift: 0}, type: 1});
function bands() { return open('bands.html', ['static/js/edit_flow.js', 'static/js/list_editor.js'], [['/api/bands', [band(1, '20m'), band(2, '17m')]]]); }
const editable = tr => [...tr.querySelectorAll('input, select, .close')].filter(i => !i.disabled).length;
scenario(() => {
  const p = bands();
  const tr = k => p.doc.querySelector('#band_list tbody tr[data-key="' + k + '"]');
  tr(1).querySelector('.edit').click();
  p.type(tr(1).querySelector('input'), '20m X');
  tr(1).querySelector('.save').click();
  check('Bands, the row\'s fields and Close while its save is on its way', {editable: editable(tr(1)), warns: p.leaving()}, {editable: 0, warns: true});
  next(p, '/api/bands/1').fail(0, 'error');
  check('Bands, the save failed', Object.assign(view(p, tr(1)), {editable: editable(tr(1))}), {values: '20m X|14|14.35|2|0|true', st: 'failed', save: true, warns: true, editable: 7, bar: '1 unsaved'});
  tr(1).querySelector('.save').click();
  next(p, '/api/bands/1').ok({});
  next(p, '/api/bands').ok([band(1, '20m X'), band(2, '17m')]);
  check('Bands, Save again, then saved', {text: tr(1).children[0].textContent, st: (tr(1).querySelector('.rst') || {getAttribute: () => null}).getAttribute('data-st'), warns: p.leaving(), bar: view(p, tr(1)).bar}, {text: '20m X', st: 'saved', warns: false, bar: ''});
});
scenario(() => {
  /* Close, Edit again and typing while the save is on its way: the table read again took that edit away */
  const p = bands();
  const tr = k => p.doc.querySelector('#band_list tbody tr[data-key="' + k + '"]');
  tr(1).querySelector('.edit').click();
  p.type(tr(1).querySelector('input'), '20m X');
  tr(1).querySelector('.save').click();
  tr(1).querySelector('.close').click();
  check('Bands, Close while the save is on its way does nothing', {editing: !!tr(1).querySelector('input'), edit: !!tr(1).querySelector('.edit')}, {editing: true, edit: false});
  next(p, '/api/bands/1').ok({});
  next(p, '/api/bands').ok([band(1, '20m X'), band(2, '17m')]);
  check('Bands, then saved', {text: tr(1).children[0].textContent, warns: p.leaving(), bar: view(p, tr(1)).bar}, {text: '20m X', warns: false, bar: ''});
});
scenario(() => {
  /* a failed save, then Close: the radio's row back, nothing left to save */
  const p = bands();
  const tr = k => p.doc.querySelector('#band_list tbody tr[data-key="' + k + '"]');
  tr(1).querySelector('.edit').click();
  p.type(tr(1).querySelector('input'), '20m X');
  tr(1).querySelector('.save').click();
  next(p, '/api/bands/1').fail(0, 'timeout');
  tr(1).querySelector('.close').click();
  check('Bands, a failed save closed', {text: tr(1).children[0].textContent, warns: p.leaving(), bar: view(p, tr(1)).bar, add: (p.doc.getElementById('add_band').click(), !!p.doc.querySelector('#band_list tbody tr[data-key=""]'))}, {text: '20m', warns: false, bar: '', add: true});
});
scenario(() => {
  /* Add while a row is being edited: the new row's save reads the table again, over the other's edit */
  const p = bands();
  const tr = k => p.doc.querySelector('#band_list tbody tr[data-key="' + k + '"]');
  tr(2).querySelector('.edit').click();
  p.type(tr(2).querySelector('input'), '17m X');
  p.doc.getElementById('add_band').click();
  const fresh = p.doc.querySelector('#band_list tbody tr[data-key=""]');
  if (fresh) { fresh.querySelector('.save').click(); next(p, '/api/bands').ok({}); next(p, '/api/bands').ok([band(1, '20m'), band(2, '17m'), band(3, 'New')]); }
  check('Bands, Add while 17m is edited, the new band saved', {edit17: tr(2) && tr(2).querySelector('input') ? tr(2).querySelector('input').value : '(gone: ' + tr(2).children[0].textContent + ')', warns: p.leaving()}, {edit17: '17m X', warns: true});
});

const bad = results.filter(ok => !ok).length;
console.log(bad ? bad + ' of ' + results.length + ' edit-flow cases wrong (audit C12)' : 'All ' + results.length + ' edit-flow cases as they should be (fixed, audit C12)');
process.exit(bad ? 1 : 0);
