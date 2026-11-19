/* RadioSession.autoOpen (radio_session.js) in a bare context with a clock of its own: the cases an app page meets as
 * it opens, each a page of its own. Prints a line a case, "FAIL ..." for one that does not hold; exits 1 then.
 *   node auto_open.js <radio_session.js>
 * SPDX-License-Identifier: LGPL-2.1-or-later */
"use strict";
const fs = require("fs"), vm = require("vm");
let now = 1000, next = 1;
const timers = new Map();
const ctx = {window: {}, Date: {now: function() { return now; }},
             setTimeout: function(fn, ms) { const id = next++; timers.set(id, {fn: fn, at: now + ms}); return id; },
             clearTimeout: function(id) { timers.delete(id); }};
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(process.argv[2], "utf8"), ctx);
const autoOpen = ctx.window.RadioSession.autoOpen;

function advance(ms) {
  const to = now + ms;
  for (;;) {
    const due = [...timers].filter(function(e) { return e[1].at <= to; }).sort(function(a, b) { return a[1].at - b[1].at; })[0];
    if (!due) break;
    timers.delete(due[0]); now = due[1].at; due[1].fn();
  }
  now = to;
}

/* A page: its app's state, the radio's, the session's can(), what it sent and its status line */
function page(o) {
  const p = {app: o.app, radio: o.radio, can: o.can || "", sent: [], line: "", error: false, names: o.names || ["FT8", "FT4", "FT2"]};
  p.tick = autoOpen({cmd: o.cmd || "ft8", names: p.names, label: o.label || "FT8",
                     app: function() { return p.app; }, radio: function() { return p.radio; },
                     can: function() { return p.can; }, send: function(s) { p.sent.push(s); return true; },
                     status: function(t, e) { p.line = t; p.error = !!e; }, shown: function() { return p.line; }});
  return p;
}

let fails = 0, cases = 0;
function check(ok, what, got) {
  cases++;
  if (!ok) { fails++; console.log("FAIL " + what + (got !== undefined ? ": " + JSON.stringify(got) : "")); }
  else console.log("ok   " + what);
}

/* 1. nothing open, paired and in control: opened once, after the wait for the role, said, the line freed after */
{
  const p = page({app: {open: false}, radio: {app: ""}});
  p.tick();
  check(p.sent.length === 0, "nothing sent before the wait", p.sent);
  advance(1000); p.tick();
  check(p.sent.length === 0, "nor a second later", p.sent);
  advance(700);
  check(p.sent.length === 1 && p.sent[0] === "ft8 open" && p.line === "Opening FT8 on the radio...",
        "\"ft8 open\" once the wait is over, by its own timer", [p.sent, p.line]);
  p.app = {open: true}; p.tick();
  check(p.line === "FT8 is open on the radio", "said when the app says it is open", p.line);
  advance(4100);
  check(p.line === "", "the line freed 4 s later", p.line);
  p.app = {open: false}; p.tick(); advance(5000); p.tick();
  check(p.sent.length === 1, "closed later (Close on the page): not opened again", p.sent);
}

/* 2. another digital app on the radio: left as it is, the page says Open switches */
{
  const p = page({cmd: "ft8", app: {open: false}, radio: {app: "JS8"}});
  p.tick(); advance(1700); p.tick();
  check(p.sent.length === 0 && p.line === "JS8 is open on the radio: Open switches it to FT8",
        "JS8 open on the radio: not taken", [p.sent, p.line]);
}

/* 3. the FT8 app open in FT2 (the radio names it so): the page finds it open, nothing to do */
{
  const p = page({app: {open: true}, radio: {app: "FT2"}});
  p.tick(); advance(2000); p.tick();
  check(p.sent.length === 0 && p.line === "", "FT8 open in FT2: nothing sent, nothing said", [p.sent, p.line]);
}

/* 4. not paired: no dialog, the reason on the line; paired later, the page does not try again by itself */
{
  const p = page({app: {open: false}, radio: {app: ""}, can: "This browser only watches: Pair it to operate"});
  p.tick(); advance(1700);
  check(p.sent.length === 0 && p.line === "FT8 is closed on the radio. This browser only watches: Pair it to operate",
        "not paired: the reason said", [p.sent, p.line]);
  p.can = ""; p.tick(); advance(2000); p.tick();
  check(p.sent.length === 0, "paired afterwards: still once only", p.sent);
}

/* 5. the role comes during the wait (auth answered): the decision takes it */
{
  const p = page({app: {open: false}, radio: {app: ""}, can: "This browser only watches: Pair it to operate"});
  p.tick(); advance(800); p.can = ""; advance(900);
  check(p.sent.length === 1 && p.sent[0] === "ft8 open", "the role known within the wait: opened", p.sent);
}

/* 6. it never opens (refused on the radio): said after 15 s, as an error */
{
  const p = page({cmd: "js8", names: ["JS8"], label: "JS8", app: {open: false}, radio: {app: ""}});
  p.tick(); advance(1700);
  check(p.sent[0] === "js8 open", "JS8's own command", p.sent);
  advance(15100); p.tick();
  check(p.line === "JS8 did not open on the radio: Open tries again" && p.error, "not open 15 s later: said",
        [p.line, p.error]);
}

/* 7. one of the two states missing (no "state" yet): it waits, the timer not even started */
{
  const p = page({cmd: "wefax", names: ["WeFax"], label: "WeFax", app: {open: false}, radio: null});
  p.tick(); advance(5000); p.tick();
  check(p.sent.length === 0 && timers.size === 0, "no radio state yet: waits", p.sent);
  p.radio = {app: ""}; p.tick(); advance(1700);
  check(p.sent[0] === "wefax open", "the state come: opened after the wait", p.sent);
}

/* 8. another message on the line by then: the 4 s clear leaves it */
{
  const p = page({app: {open: false}, radio: {app: ""}});
  p.tick(); advance(1700); p.app = {open: true}; p.tick();
  p.line = "Queued: DL1ABC ON6URE JO21";
  advance(4100);
  check(p.line === "Queued: DL1ABC ON6URE JO21", "a newer message is not cleared", p.line);
}

console.log("auto_open: " + cases + " checks, " + fails + " failed");
process.exit(fails ? 1 : 0);
