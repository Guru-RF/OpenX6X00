/* app_wall.js's country() on each call on stdin, as dxcc_calls prints src/dxcc.c's: "CALL<tab>name<tab>primary
 * prefix", "-" for both when unknown. The page's script in a bare context, its fetches answered from files:
 * node country_js.js <app_wall.js> <the /api/dxcc table> <flags.json> < calls */
"use strict";
const fs = require("fs"), vm = require("vm");
const [src, table, flags] = process.argv.slice(2);
const files = {"/api/dxcc": table, "/static/img/flags.json?v=1": flags};
const ctx = {
  window: {fetch: true, addEventListener: function() {}, innerWidth: 1200},
  document: {createElement: function() { return {style: {}}; }},
  fetch: function(url) { return Promise.resolve({json: function() { return JSON.parse(fs.readFileSync(files[url], "utf8")); }}); },
};
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(src, "utf8") + "\nthis.AppWall = AppWall;", ctx);
ctx.AppWall.ready(function() {
  const out = fs.readFileSync(0, "utf8").split("\n").filter(Boolean).map(function(call) {
    const c = ctx.AppWall.country(call);
    return call + "\t" + (c ? c.k + "\t" + c.pfx : "-\t-");
  });
  process.stdout.write(out.join("\n") + "\n");
});
