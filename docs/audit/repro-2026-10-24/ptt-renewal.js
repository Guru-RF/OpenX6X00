const fs = require('fs');
const vm = require('vm');
let now = 0, next = 1;
const timers = new Map(), sent = [];
function add(fn, ms, repeat) { const id = next++; timers.set(id, {fn, at: now + ms, ms, repeat}); return id; }
const context = {window: {}, Date: {now: () => now}, setTimeout: (fn, ms) => add(fn, ms, false), setInterval: (fn, ms) => add(fn, ms, true), clearTimeout: id => timers.delete(id), clearInterval: id => timers.delete(id)};
vm.runInNewContext(fs.readFileSync('webserver/src/x6100_webserver/static/js/radio_session.js', 'utf8'), context);
function advance(to) {
  while (true) {
    const entry = [...timers].filter(([id, t]) => t.at <= to).sort((a, b) => a[1].at - b[1].at)[0];
    if (!entry) break;
    const [id, t] = entry; now = t.at;
    if (t.repeat) t.at += t.ms; else timers.delete(id);
    t.fn();
  }
  now = to;
}
const ptt = context.window.RadioPtt.create({el: null, send: text => {sent.push({at: now, text}); return true;}, session: {can: () => ''}, mic: {wanted: () => false}});
ptt.press();
advance(100);
ptt.radioTx(true);
advance(4000);
console.log(JSON.stringify({state: ptt.state, sent, renewTimer: ptt.renew, expected: 'key PTT 1 once per second while held'}, null, 2));
if (sent.filter(x => x.text === 'key PTT 1').length !== 1) process.exit(1);
