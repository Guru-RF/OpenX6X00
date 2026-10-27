/*
 * RadioSession: who this browser is to the radio, and who controls the radio (R3); RadioPtt: a
 * transmission as the radio confirms it, never as the page hopes (R4). Shared by the Panel and
 * Remote pages, on top of RadioLink.
 *
 *   const ses = RadioSession.create({send, status, el, onChange});
 *     ses.onOpen()                  after "panel 1": sends "auth TOKEN" when this browser was paired
 *     ses.onText(head, body)        -> true when the message was its own (role, ctl, paired, tx, sessions)
 *     ses.role                      "view" | "operate" | "admin";  ses.tx (may transmit);  ses.name
 *     ses.ctl                       {who: "none"|"local"|"tci"|"browser", name, mine}
 *     ses.radioTx                   the radio's TX state as last told ("tx 0|1")
 *     ses.can()                     -> "" when this browser may operate now, else the reason
 *     ses.explain()                 the dialog that changes what can() refused (pair, take control)
 *     ses.chips()                   -> [{n, s, t}] for RadioLink.health: Access and Control
 *   The element `el` gets two buttons: the role (Pair, or this browser's name) and the control
 *   holder (Take control / Release); an admin gets Sessions. The token lives in localStorage
 *   "x6100session", so the Panel and the Remote page share one pairing.
 *   RadioSession.forRadio(keyEvent, pttEl) -> true when a key is for the page's radio shortcuts
 *                                 rather than for a field, a focused control or an open dialog
 *
 *   const ptt = RadioPtt.create({el, send, status, mic: {wanted, ready, start, stop}, session, link});
 *     ptt.press() / ptt.release()   the operator's thumb; press is refused when the mic is wanted
 *                                   but not ready (never a silent fallback to the radio's microphone),
 *                                   and while the link (a RadioLink) is stalled
 *     ptt.radioTx(on)               the confirmed TX state ("tx 1|0", state.tx)
 *     ptt.err(text)                 an "err ..." from the radio: a refused or ended transmission
 *     ptt.lost(why)                 the link went or stalled: whatever was on the air is over (the
 *                                   release goes out when the socket still takes it)
 *     ptt.state                     idle | requesting | tx | releasing | ended | refused | busy
 *   While a radio-microphone PTT is held the key is renewed every second (the radio lets it go
 *   after three seconds without, and after three minutes whatever happens): "key PTT 1 N", N the
 *   press's own number. The radio renews only the hold that keyed, so renewals that arrive after
 *   it let go (a stalled link delivering late) never key it again (audit C02).
 */
(function() {
  "use strict";
  const TOKEN_KEY = "x6100session", NAME_KEY = "x6100name";
  function store(k, v) { try { if (v === null) localStorage.removeItem(k); else localStorage.setItem(k, v); } catch (e) {} }
  function load(k) { try { return localStorage.getItem(k); } catch (e) { return null; } }
  function age(s) { return s < 60 ? "just now" : s < 3600 ? Math.floor(s / 60) + " min ago" : s < 86400 ? Math.floor(s / 3600) + " h ago" : Math.floor(s / 86400) + " d ago"; }

  /* A small modal of our own: title, body (jQuery or text), buttons */
  let $pop = null;
  function popup(title, body, buttons) {
    if (!$pop) { $pop = $("<div class='spop' hidden><div class='sbox'></div></div>").appendTo(document.body).on("pointerdown", function(e) { if (e.target === this) popClose(); }); }
    const $b = $pop.children(".sbox").empty();
    $b.append($("<h3>").text(title), $("<div class='sbody'>").append(body));
    const $a = $("<div class='actions'>").appendTo($b);
    (buttons || []).forEach(function(bt) { $a.append($("<button type='button'>").addClass(bt.primary ? "primary" : "").text(bt.t).on("click", function() { if (bt.cb) bt.cb(); if (!bt.keep) popClose(); })); });
    $pop.prop("hidden", false);
    const f = $b.find("input").first(); if (f.length) f.trigger("focus");
    return $b;
  }
  function popClose() { if ($pop) { $pop.prop("hidden", true); $pop.children(".sbox").empty(); } }

  /* A key for the page's radio shortcuts (Space PTT, Enter the MFK push, the arrows...) only when it is meant
   * for nothing else (audit C09): none while our dialog is open, none typed into a field or a list, and Space
   * or Enter only while no control has the focus, since they are what presses a focused button, link or tab:
   * Tab to Sound and Space is Sound, never PTT. Space on the PTT button itself (pttEl) is PTT */
  const FIELDS = "input, textarea, select, [role=textbox], [role=combobox], [role=listbox], [role=slider], [role=spinbutton]";
  function forRadio(e, pttEl) {
    if (e.defaultPrevented || ($pop && !$pop.prop("hidden"))) return false;
    const t = e.target;
    if (t.isContentEditable || t.closest(FIELDS)) return false;
    if (e.key !== " " && e.key !== "Enter") return true;
    return t === document.body || t === document.documentElement || (e.key === " " && !!pttEl && t === pttEl);
  }

  function Session(o) {
    this.o = o; this.role = "view"; this.tx = false; this.name = "-"; this.pending = false; this.radioTx = false;
    this.ctl = {who: "none", name: "", mine: false}; this.sessions = null; this.pairing = false;
    this.$role = $("<button type='button' class='ses'>").on("click", this.roleClick.bind(this));
    this.$ctl = $("<button type='button' class='ses'>").on("click", this.ctlClick.bind(this));
    this.$adm = $("<button type='button' class='ses' hidden>Sessions</button>").on("click", this.sessionsOpen.bind(this));
    $(o.el).empty().append(this.$role, this.$ctl, this.$adm);
    this.render();
  }
  Session.prototype.onOpen = function() {
    const tok = load(TOKEN_KEY);
    if (tok) { this.pending = true; this.o.send("auth " + tok); }
    else this.pending = false;
  };
  Session.prototype.onText = function(head, body) {
    const w = body.split(" ");
    if (head === "role") {
      if (this.pending && w[0] === "view") return true;      /* the greeting: our auth is on its way */
      this.pending = false;
      this.role = w[0]; this.tx = w[1] === "1"; this.name = w.slice(2).join(" ") || "-";
      this.render(); if (this.o.onChange) this.o.onChange();
      return true;
    }
    if (head === "ctl") {
      /* "ctl none|local|tci 0" or "ctl browser NAME 0|1" */
      const mine = w[w.length - 1] === "1", who = w[0], name = who === "browser" ? w.slice(1, -1).join(" ") : "";
      const was = this.ctl;
      this.ctl = {who: who, name: name, mine: mine};
      if (was.mine && !mine && this.o.status) this.o.status(who === "local" ? "Control taken at the radio: Take control to operate from here again" : who === "tci" ? "A TCI client took control: Take control to operate from here again" : who === "browser" ? "Control taken by " + name : "Control released", who !== "none");
      this.render(); if (this.o.onChange) this.o.onChange();
      return true;
    }
    if (head === "paired") {
      store(TOKEN_KEY, w[0]); this.pending = false; this.pairing = false; popClose();
      if (this.o.status) this.o.status("Paired as " + (w[1] === "admin" ? "the admin: this browser may operate and manage the others" : "an operator: this browser may operate the radio"));
      return true;
    }
    if (head === "tx") { this.radioTx = w[0] === "1"; if (this.o.onTx) this.o.onTx(this.radioTx); return true; }
    if (head === "sessions") { try { this.sessions = JSON.parse(body); } catch (e) { this.sessions = []; } this.sessionsRender(); return true; }
    if (head === "err") {
      /* A key, a knob or a setting refused: say how to get it at once, in a dialog, rather than in the status
       * line, where people did not find it ("how do I take control?"). Not more than once in 4 s: a held key or a
       * turned knob repeats */
      if (/^view only/.test(body) && !this.pairing) {
        if (Date.now() - (this.askedAt || 0) > 4000) { this.askedAt = Date.now(); this.pairError = null; this.pairOpen(); }
        return false;    /* the page still sees it: the PTT button shows the refusal */
      }
      if (/^controlled by/.test(body)) {
        if (Date.now() - (this.askedAt || 0) > 4000) { this.askedAt = Date.now(); this.takeOpen(); }
        return false;
      }
      if (/^unknown session/.test(body)) { store(TOKEN_KEY, null); this.pending = false; this.role = "view"; this.render(); if (this.o.onChange) this.o.onChange(); }
      else if (/^pairing refused/.test(body) && this.pairing) { this.pairError = body.replace(/^pairing refused: /, ""); this.pairOpen(); }
      else if (/^session revoked/.test(body)) { store(TOKEN_KEY, null); this.role = "view"; this.render(); }
      return false;    /* the page shows it too */
    }
    return false;
  };
  Session.prototype.can = function() {
    if (this.role === "view") return "This browser only watches: Pair it to operate";
    if (this.ctl.who !== "none" && !this.ctl.mine) return this.ctl.who === "local" ? "The radio is operated at the radio: Take control first" : this.ctl.who === "tci" ? "A TCI client operates the radio: Take control first" : "Controlled by " + this.ctl.name + ": Take control first";
    return "";
  };
  /* What can() refused, as the radio's own refusal is shown: the pairing or the Take control dialog, not more
   * than once in 4 s (a page that knows the answer does not send, audit C13) */
  Session.prototype.explain = function() {
    if (Date.now() - (this.askedAt || 0) <= 4000) return;
    this.askedAt = Date.now();
    if (this.role === "view") { if (!this.pairing) { this.pairError = null; this.pairOpen(); } }
    else if (this.ctl.who !== "none" && !this.ctl.mine) this.takeOpen();
  };
  Session.prototype.chips = function() {
    const c = this.ctl;
    const ctlChip = c.mine ? {n: "Control", s: "ok", t: "this browser"} : c.who === "none" ? {n: "Control", s: "idle", t: "free"} : c.who === "local" ? {n: "Control", s: "stale", t: "at the radio"} : c.who === "tci" ? {n: "Control", s: "stale", t: "TCI client"} : {n: "Control", s: "stale", t: c.name};
    const roleChip = this.role === "view" ? {n: "Access", s: "off", t: "view only"} : this.role === "admin" ? {n: "Access", s: "ok", t: "admin"} : {n: "Access", s: "ok", t: "operate"};
    return [roleChip, ctlChip];
  };
  Session.prototype.render = function() {
    const c = this.ctl;
    this.$role.text(this.role === "view" ? "View only · Pair to operate" : this.name + (this.role === "admin" ? " · admin" : "")).toggleClass("warn", this.role === "view").attr("title", this.role === "view" ? "This browser only watches. Pair it with a code shown on the radio to operate" : "This browser is paired and may operate the radio");
    let t, title;
    if (c.mine) { t = "In control · Release"; title = "This browser controls the radio. Release lets another take it without a takeover"; }
    else if (c.who === "none") { t = "Control: free"; title = "Nobody controls the radio: the first to tune or key takes it"; }
    else if (c.who === "local") { t = "At the radio · Take control"; title = "Someone used the radio's own keys or knobs. Take control to operate from here"; }
    else if (c.who === "tci") { t = "TCI client · Take control"; title = "A TCI client (a VFO knob, a program) operates the radio. Take control to operate from here"; }
    else { t = c.name + " · Take control"; title = "Another browser operates the radio. Take control moves it here (not while the radio transmits)"; }
    this.$ctl.text(t).attr("title", title).toggleClass("ok", c.mine).toggleClass("warn", !c.mine && c.who !== "none").prop("disabled", this.role === "view" && c.who === "none");
    this.$adm.prop("hidden", this.role !== "admin");
  };
  Session.prototype.roleClick = function() {
    if (this.role === "view") { this.pairError = null; this.pairOpen(); return; }
    const self = this;
    popup("This browser", $("<p>").text("Paired as \"" + this.name + "\" (" + (this.role === "admin" ? "admin: operates the radio and manages the paired browsers" : "operator: operates the radio") + ")."), [
      {t: "Unpair this browser", cb: function() { if (confirm("Unpair this browser? It only watches until it is paired again.")) { store(TOKEN_KEY, null); self.o.send("unpair"); } }},
      {t: "Close", primary: true},
    ]);
  };
  /* "Take control": who has it now, and one button to bring it here */
  Session.prototype.takeOpen = function() {
    const self = this, c = this.ctl;
    const who = c.who === "local" ? "Someone used the radio's own keys or knobs, so the radio is operated at the radio."
              : c.who === "tci" ? "A TCI client (a VFO knob, a logging program) operates the radio."
              : c.who === "browser" ? "\"" + c.name + "\" operates the radio from another browser." : "Another place operates the radio.";
    popup("Take control", $("<div>").append(
      $("<p>").text(who + " One place at a time operates the radio; this browser only watches until it takes control."),
      $("<p>").text("Take control moves it here. The button at the top (\"Take control\") does the same at any time; at the radio, a key or a knob takes it back.")),
      [{t: "Take control", primary: true, cb: function() { self.ctlClick(); }}, {t: "Not now"}]);
  };
  Session.prototype.pairOpen = function() {
    const self = this;
    if (!this.pairing) { this.pairing = true; this.o.send("pair?"); }
    const code = $("<input inputmode='numeric' pattern='[0-9]*' maxlength='6' autocomplete='off' placeholder='6 digits'>");
    const name = $("<input maxlength='24' autocomplete='off' placeholder='for example Kitchen phone'>").val(load(NAME_KEY) || "");
    const body = $("<div>").append(
      $("<p>").text("This browser only watches until it is paired, once. A six-digit code is now on the radio's screen for two minutes (Settings › General › BROWSER REMOTE › Pair code shows it again). Type it here, and a name for this browser."),
      $("<label>").text("Code").append(code), $("<label>").text("This browser's name").append(name),
      this.pairError ? $("<p class='err'>").text(this.pairError) : "");
    const go = function() {
      const c = code.val().trim(), n = name.val().trim();
      if (!/^\d{6}$/.test(c)) { code.trigger("focus"); return; }
      if (!n) { name.trigger("focus"); return; }
      store(NAME_KEY, n); self.pairError = null;
      self.o.send("pair " + c + " " + n);
    };
    code.add(name).on("keydown", function(e) { if (e.key === "Enter") { e.preventDefault(); go(); } });
    popup("Pair this browser", body, [{t: "Pair", primary: true, cb: go, keep: true}, {t: "Cancel", cb: function() { self.pairing = false; }}]);
  };
  Session.prototype.ctlClick = function() {
    if (this.role === "view") { this.pairError = null; this.pairOpen(); return; }
    if (this.ctl.mine) this.o.send("control release");
    else if (this.radioTx) { if (this.o.status) this.o.status("The radio is transmitting: control cannot move now", true); }
    else this.o.send("control take");
  };
  Session.prototype.sessionsOpen = function() { this.sessions = null; this.o.send("sessions"); this.sessionsRender(); };
  Session.prototype.sessionsRender = function() {
    if (!$pop || $pop.prop("hidden") && this.sessions === null) { /* opening */ }
    const self = this, list = $("<div class='slist'>");
    if (this.sessions === null) list.text("Asking the radio...");
    else if (!this.sessions.length) list.text("No browser is paired.");
    else this.sessions.forEach(function(s) {
      const row = $("<div class='srow'>").append(
        $("<b>").text(s.name + (s.me ? " (this browser)" : "")),
        $("<span>").text(s.role + (s.online ? " · connected" : " · seen " + age(Math.max(0, Date.now() / 1000 - s.last))) + " · " + s.ip),
        $("<button type='button'>").text(s.me ? "Unpair" : "Revoke").on("click", function() {
          if (!confirm((s.me ? "Unpair this browser?" : "Revoke \"" + s.name + "\"?") + " Its connections close now and it only watches until it is paired again.")) return;
          if (s.me) store(TOKEN_KEY, null);
          self.o.send("revoke " + s.n);
        }));
      list.append(row);
    });
    popup("Paired browsers", $("<div>").append($("<p>").text("Each paired browser may operate the radio; the admin (the first paired) manages this list. Revoke closes a browser's connections at once."), list), [{t: "Close", primary: true}]);
  };

  /* ---- PTT ---- */
  const KEY_MS = 1500, RENEW_MS = 1000, ENDED_MS = 5000;
  function Ptt(o) {
    this.o = o; this.state = "idle"; this.path = null; this.since = 0; this.timer = null; this.renew = null; this.why = ""; this.tick = null;
    this.hold = 0;    /* the last press's number */
    const self = this;
    this.tick = setInterval(function() { if (self.state === "tx") self.render(); }, 1000);
    this.render();
  }
  Ptt.prototype.clear = function() { if (this.timer) { clearTimeout(this.timer); this.timer = null; } if (this.renew) { clearInterval(this.renew); this.renew = null; } };
  Ptt.prototype.press = function() {
    const self = this, o = this.o;
    if (this.state === "requesting" || this.state === "tx") return;
    if (this.state === "releasing") { this.why = "Wait: the radio is still unkeying"; this.render(); return; }
    if (this.state === "busy") { this.set("busy", "The radio is transmitting already"); return; }
    if (o.link && o.link.stalled) { this.set("refused", "The radio does not answer: no transmission until it does"); return; }
    const no = o.session ? o.session.can() : "";
    if (no) { this.set("refused", no); return; }
    const mic = o.mic && o.mic.wanted();
    if (mic && !o.mic.ready()) { this.set("refused", "Microphone not ready: no transmission (the radio's own microphone is not used in its place)"); return; }
    this.path = mic ? "mic" : "radio"; this.since = Date.now();
    this.clear();
    let sent;
    if (mic) sent = o.mic.start();
    else { const key = "key PTT 1 " + (++this.hold); sent = o.send(key); if (sent) this.renew = setInterval(function() { o.send(key); }, RENEW_MS); }
    if (!sent) { this.set("refused", "No link to the radio: no transmission"); return; }
    this.state = "requesting"; this.why = ""; this.render();
    this.timer = setTimeout(function() { if (self.state === "requesting") { self.stopPath(); self.set("refused", "The radio did not key (no TX confirmed in 1.5 s)"); } }, KEY_MS);
  };
  Ptt.prototype.stopPath = function() {
    if (this.renew) { clearInterval(this.renew); this.renew = null; }
    if (this.path === "mic") this.o.mic.stop(); else this.o.send("key PTT 0");
  };
  Ptt.prototype.release = function() {
    const self = this;
    if (this.state !== "requesting" && this.state !== "tx") return;
    this.clear();
    this.stopPath();
    this.state = "releasing"; this.render();
    this.timer = setTimeout(function() { if (self.state === "releasing") self.set("idle", ""); }, KEY_MS);
  };
  Ptt.prototype.radioTx = function(on) {
    const self = this;
    if (on) {
      /* only the confirmation timeout goes: the renewal keeps the server's lease while the button is held (audit B10) */
      if (this.state === "requesting") { if (this.timer) { clearTimeout(this.timer); this.timer = null; } this.state = "tx"; this.why = ""; this.render(); if (this.o.status) this.o.status("On the air (" + (this.path === "mic" ? "browser microphone" : "the radio's microphone") + ")"); }
      else if (this.state === "idle" || this.state === "ended" || this.state === "refused") { this.state = "busy"; this.render(); }
    } else {
      if (this.state === "tx") { this.clear(); this.stopPath(); this.set("ended", "The radio ended the transmission"); }
      else if (this.state === "releasing" || this.state === "busy") this.set("idle", "");
    }
    if (!on && this.state === "ended" && !this.timer) this.timer = setTimeout(function() { if (self.state === "ended") self.set("idle", ""); }, ENDED_MS);
  };
  /* An err while a press waits for "tx 1": the PTT's own refusals (view only, control elsewhere, no TX for this
   * session) end it, and let go as the 1.5 s timeout does: an err cannot say which command it answers, and this
   * press may have keyed. Another err (a tune refused, a Take control refused while on the air) leaves it */
  Ptt.prototype.err = function(text) {
    if (/^ptt released/.test(text)) { this.clear(); if (this.renew) { clearInterval(this.renew); this.renew = null; } if (this.state === "tx" || this.state === "requesting") this.set("ended", "The radio unkeyed: " + text.replace(/^ptt released: /, "")); return true; }
    if (this.state === "requesting" && /^(controlled by|view only|this session may not transmit)/.test(text)) { this.clear(); this.stopPath(); this.set("refused", text); return true; }
    return false;
  };
  /* The setting changed (the microphone on or off): an old refusal or ending is no longer the news */
  Ptt.prototype.reset = function() { if (this.state === "refused" || this.state === "ended") this.set("idle", ""); else this.render(); };
  Ptt.prototype.lost = function(why) {
    if (this.state === "requesting" || this.state === "tx" || this.state === "releasing") { this.clear(); this.stopPath(); this.set("ended", why || "The link dropped: the transmission is over"); }
  };
  Ptt.prototype.set = function(state, why) {
    const self = this;
    this.clear();
    this.state = state; this.why = why || "";
    if (why && this.o.status) this.o.status(why, state !== "idle");
    if (state === "ended" || state === "refused") this.timer = setTimeout(function() { if (self.state === state) self.set("idle", ""); }, ENDED_MS);
    this.render();
  };
  Ptt.prototype.render = function() {
    const el = this.o.el; if (!el) return;
    const mic = this.o.mic && this.o.mic.wanted();
    let main, sub = "TX audio: " + (mic ? "browser microphone" : "microphone at the radio");
    switch (this.state) {
      case "requesting": main = "Keying..."; break;
      case "tx": { const s = Math.floor((Date.now() - this.since) / 1000); main = "ON AIR " + Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0"); sub = this.path === "mic" ? "browser microphone" : "microphone at the radio"; break; }
      case "releasing": main = "Unkeying..."; break;
      case "ended": main = "TX ended"; sub = "press again to transmit"; break;
      case "refused": main = "No TX"; sub = this.why.length > 48 ? this.why.slice(0, 46) + "…" : this.why; break;
      case "busy": main = "TX (not this browser)"; sub = "the radio is on the air"; break;
      default: main = "PTT (hold)";
    }
    el.classList.toggle("down", this.state === "tx" || this.state === "requesting");
    el.classList.toggle("warn", this.state === "ended" || this.state === "refused" || this.state === "busy");
    el.innerHTML = ""; el.appendChild(document.createTextNode(main)); const sm = document.createElement("small"); sm.textContent = sub; el.appendChild(sm);
  };

  window.RadioSession = {create: function(o) { return new Session(o); }, forRadio: forRadio};
  window.RadioPtt = {create: function(o) { return new Ptt(o); }};
})();
