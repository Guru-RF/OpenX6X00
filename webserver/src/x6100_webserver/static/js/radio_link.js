/* The browser pages' link to the radio, and what the page says about it (R6).
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * A Link is one WebSocket that comes back by itself: the first try after a second, then two, four,
 * eight, at most fifteen seconds apart, and at once when the page asks (tryNow). Every connection
 * is a new generation: what the page had in flight on the old one is gone, never sent again. The
 * radio's "hello <version> <boot>" tells which GUI run answers; a changed boot means the radio
 * restarted and nothing it had been asked survives. "ping" goes every five seconds; a "pong" that
 * does not come within the next three marks the link stalled: the socket is up, but nothing has
 * come through, so what the page shows is old.
 *
 *   const link = RadioLink.create({
 *     url: function() { return "ws://..."; },  // or null (not now: remote off), or a promise of either
 *     ping: "ping" | null,                   // the text that is answered "pong" (none for the TCI)
 *     onOpen(gen), onText(text, gen), onBinary(buf, gen), onClose(gen, willRetry), onHello(boot, restarted)
 *   });
 *   link.connect(); link.send(text) -> sent?; link.up; link.stalled; link.gen; link.boot; link.lastRx;
 *   link.retryAt (ms, when the next try is) ; link.tryNow(); link.close(); link.tries
 *
 * RadioLink.health(el, [{n: "Control", s: "ok"|"stale"|"off"|"idle", t: "12 fps"}]) draws the channels
 * as chips in el. RadioLink.age(ms) gives "3 s" / "2 min" / "1 h".
 */
window.RadioLink = (function() {
  "use strict";
  const BACKOFF = [1000, 2000, 4000, 8000, 15000];
  const PING_MS = 5000, PONG_MS = 3000;

  function Link(o) {
    this.o = o; this.ws = null; this.up = false; this.stalled = false; this.gen = 0; this.boot = null;
    this.lastRx = 0; this.lastPong = 0; this.pingAt = 0; this.tries = 0; this.retryAt = 0; this.alive = true;
    this.timer = null; this.pinger = null; this.opening = false;
  }
  Link.prototype.connect = function() {
    const self = this;
    if (!this.alive || this.ws || this.opening) return;
    if (this.timer) { clearTimeout(this.timer); this.timer = null; }
    this.opening = true;
    Promise.resolve(this.o.url()).then(function(url) { self.opening = false; self.open(url); }, function() { self.opening = false; self.schedule(); });
  };
  Link.prototype.open = function(url) {
    const self = this;
    if (!this.alive || this.ws) return;
    if (!url) { this.schedule(); return; }
    const gen = ++this.gen;
    let ws;
    try { ws = new WebSocket(url); } catch (e) { this.schedule(); return; }
    ws.binaryType = "arraybuffer";
    this.ws = ws; this.retryAt = 0;
    ws.onopen = function() {
      if (self.ws !== ws) return;
      self.up = true; self.stalled = false; self.tries = 0; self.lastRx = self.lastPong = Date.now(); self.pingAt = 0;
      if (self.o.ping && !self.pinger) self.pinger = setInterval(function() { self.tick(); }, 1000);
      if (self.o.onOpen) self.o.onOpen(gen);
    };
    ws.onmessage = function(ev) {
      if (self.ws !== ws) return;
      self.lastRx = Date.now();
      if (self.stalled) { self.stalled = false; if (self.o.onStall) self.o.onStall(false); }
      if (ev.data instanceof ArrayBuffer) { if (self.o.onBinary) self.o.onBinary(ev.data, gen); return; }
      const t = String(ev.data);
      if (t === "pong") { self.lastPong = Date.now(); self.pingAt = 0; return; }
      if (t.slice(0, 6) === "hello ") {
        /* "hello <version words...> <boot>": the boot is the last word, a number; an older radio has none */
        const w = t.split(" "), last = w[w.length - 1], boot = w.length > 2 && /^\d+$/.test(last) ? last : null;
        const restarted = !!(boot && self.boot && boot !== self.boot);
        self.boot = boot;
        if (self.o.onHello) self.o.onHello(boot, restarted, gen);
      }
      if (self.o.onText) self.o.onText(t, gen);
    };
    ws.onclose = function() {
      if (self.ws !== ws) return;
      const wasUp = self.up;
      self.ws = null; self.up = false; self.stalled = false;
      if (self.pinger) { clearInterval(self.pinger); self.pinger = null; }
      if (self.o.onClose) self.o.onClose(gen, self.alive, wasUp);
      self.schedule();
    };
    ws.onerror = function() {};
  };
  Link.prototype.schedule = function() {
    const self = this;
    if (!this.alive || this.timer) return;
    const wait = BACKOFF[Math.min(this.tries, BACKOFF.length - 1)];
    this.tries++;
    this.retryAt = Date.now() + wait;
    this.timer = setTimeout(function() { self.timer = null; self.connect(); }, wait);
  };
  Link.prototype.tryNow = function() {
    if (this.ws || this.opening) return;
    if (this.timer) { clearTimeout(this.timer); this.timer = null; }
    this.connect();
  };
  Link.prototype.tick = function() {
    if (!this.up) return;
    const now = Date.now();
    if (this.pingAt && now - this.pingAt > PONG_MS && !this.stalled) { this.stalled = true; if (this.o.onStall) this.o.onStall(true); }
    if (now - this.lastPong >= PING_MS && !this.pingAt) { this.pingAt = now; this.send(this.o.ping); }
  };
  Link.prototype.send = function(s) {
    if (!this.ws || this.ws.readyState !== 1) return false;
    try { this.ws.send(s); } catch (e) { return false; }
    return true;
  };
  Link.prototype.close = function() {
    this.alive = false;
    if (this.timer) { clearTimeout(this.timer); this.timer = null; }
    if (this.pinger) { clearInterval(this.pinger); this.pinger = null; }
    if (this.ws) { try { this.ws.close(); } catch (e) {} }
  };
  /* The next try, in whole seconds from now (0: now, or connected) */
  Link.prototype.retryIn = function() { return this.ws || !this.retryAt ? 0 : Math.max(0, Math.ceil((this.retryAt - Date.now()) / 1000)); };

  function age(ms) {
    const s = Math.max(0, Math.round(ms / 1000));
    if (s < 60) return s + " s";
    if (s < 3600) return Math.floor(s / 60) + " min";
    return Math.floor(s / 3600) + " h";
  }
  function health(el, chans) {
    const frag = document.createDocumentFragment();
    chans.forEach(function(c) {
      const sp = document.createElement("span");
      sp.className = "hc " + c.s;
      sp.textContent = c.n + (c.t ? " " + c.t : "");
      sp.title = c.s === "ok" ? "Flowing" : c.s === "stale" ? "Connected, but nothing has come through: what is shown is old" : c.s === "off" ? "Not connected" : "Off";
      frag.appendChild(sp);
    });
    el.innerHTML = ""; el.appendChild(frag);
  }
  return {create: function(o) { return new Link(o); }, age: age, health: health};
})();
