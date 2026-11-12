/*
 * RadioSession: who this browser is to the radio, and who controls the radio (R3); RadioPtt: a
 * transmission as the radio confirms it, never as the page hopes (R4). Shared by the Panel and
 * Remote pages, on top of RadioLink.
 *
 *   const ses = RadioSession.create({send, status, el, onChange});
 *     ses.onOpen()                  first on a connection: sends "auth TOKEN" when this browser was paired
 *                                   (over Tailscale Funnel the radio takes nothing before it)
 *     ses.onText(head, body)        -> true when the message was its own (role, ctl, paired, tx, sessions)
 *     ses.role                      "view" | "operate" | "admin";  ses.tx (may transmit);  ses.name
 *     ses.ctl                       {who: "none"|"local"|"tci"|"browser", name, mine}
 *     ses.radioTx                   the radio's TX state as last told ("tx 0|1")
 *     ses.can()                     -> "" when this browser may operate now, else the reason
 *     ses.explain()                 the dialog that changes what can() refused (pair, take control)
 *     ses.chips()                   -> [{n, s, t}] for RadioLink.health: Access and Control
 *     ses.holds(err)                how long the radio's refusal "err TEXT" holds: a function, true while this
 *                                   browser may still not do it (view only, controlled elsewhere), or null
 *   status(text, error, holds), the page's: a text that says how things are (Take control first, On the
 *   air) comes with holds(), true while it is so, and the page's status line drops it once it is not (#58)
 *   The element `el` gets two buttons: the role (Pair, or this browser's name) and the control
 *   holder (Take control / Release); an admin gets Sessions. The token lives in localStorage
 *   "x6100session", so the Panel and the Remote page share one pairing. With awayUrl (a function
 *   giving the Panel's address on the internet while Tailscale Funnel is on, #58) a paired
 *   browser gets Use away from home: "away" -> "away TOKEN", then a link to awayUrl#t=TOKEN.
 *   RadioSession.forRadio(keyEvent, pttEl) -> true when a key is for the page's radio shortcuts
 *                                 rather than for a field, a focused control or an open dialog
 *   RadioSession.takeLink()       the Funnel page: the away link's "#t=KEY" stored, and taken out of the
 *                                 address bar (#58);  RadioSession.token(): this browser's key, null none
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
 *     ptt.micOver()                 a Mic on transmission, press to confirmed end: its TCI link holds
 *                                   control meanwhile, which the session does not report (#97)
 *   While a radio-microphone PTT is held the key is renewed every second (the radio lets it go
 *   after three seconds without, and after three minutes whatever happens): "key PTT 1 N", N the
 *   press's own number. The radio renews only the hold that keyed, so renewals that arrive after
 *   it let go (a stalled link delivering late) never key it again (audit C02).
 *
 *   const hands = RadioPttHands.create({ptt, el, status, mic: {on, stream, ctx}});   PTT options (#241), per browser in
 *                                 localStorage "x6100ptt", all off until chosen: the PTT button tapped on and off, a Bluetooth
 *                                 headset's button (play/pause, next, previous; a speaker mic's PTT, forward and backward),
 *                                 a headset's mute (digital silence, the boom raised) as PTT with Mic on. el gets the choices
 *     hands.tap()                   -> true: the PTT button is tapped on and off, hands.toggle() then
 *   Nothing keys while the page is out of sight; mic.on/stream/ctx: Mic on, its MediaStream, the page's AudioContext.
 *
 *   const tune = RadioTune.create({el, send, status, session, link});   TUNE for an external tuner (#85)
 *     tune.press()                  a tune, or the one on the air stopped (also one started at the radio)
 *     tune.radio(on, limit)         the radio's state: "tune" and "tune_s" of "state {...}"
 *     tune.err(text)                -> true when the "err ..." was the tune's (refused, stopped by the radio)
 *     tune.lost(why)                the link went or stalled: the renewals stop, the radio stops it within 3 s
 *     tune.state                    idle | requesting | on | stopping | ended | refused
 *   A press, not a hold: while the tune is on the page renews it every second ("tune 1 N", N the press's own
 *   number, as the PTT's), and the radio stops it after three seconds without, or at its own time limit.
 */
(function() {
  "use strict";
  const TOKEN_KEY = "x6100session", NAME_KEY = "x6100name";
  function store(k, v) { try { if (v === null) localStorage.removeItem(k); else localStorage.setItem(k, v); } catch (e) {} }
  function load(k) { try { return localStorage.getItem(k); } catch (e) { return null; } }

  /* Tailscale Funnel (#58): the away link (Use away from home) brings this browser's key as "#t=KEY". It goes into
   * this address's storage, and out of the address bar at once: it is neither bookmarked, nor shown, nor sent to any
   * server (a fragment never is). Only on the Funnel page: a link to the radio at home cannot plant a key there */
  function takeLink() {
    if (!/^#t=/.test(location.hash)) return false;
    const m = /^#t=([0-9a-f]{32})$/.exec(location.hash);
    if (m) store(TOKEN_KEY, m[1]);
    try { history.replaceState(null, "", location.pathname + location.search); } catch (e) { location.hash = ""; }
    return !!m;
  }
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
    this.taking = false;     /* a Take control sent: its answer says so in the status line */
    this.$role = $("<button type='button' class='ses'>").on("click", this.roleClick.bind(this));
    this.$ctl = $("<button type='button' class='ses'>").on("click", this.ctlClick.bind(this));
    this.$adm = $("<button type='button' class='ses' hidden>Sessions</button>").on("click", this.sessionsOpen.bind(this));
    this.$away = $("<button type='button' class='ses' hidden>Use away from home</button>").on("click", this.awayOpen.bind(this));
    $(o.el).empty().append(this.$role, this.$ctl, this.$adm, this.$away);
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
      const was = this.ctl, self = this, took = this.taking;
      this.ctl = {who: who, name: name, mine: mine}; this.taking = false;
      /* This page's own Mic on keys through its TCI link, which holds control for the over and gives it back after
       * (#97): not a takeover to report, nor a Take control to ask for */
      const own = who === "tci" && this.ptt && this.ptt.micOver();
      /* what the status line says of it goes when control moves again (#58: "Take control first" stayed up) */
      const still = function() { return self.ctl.who === who && self.ctl.name === name && self.ctl.mine === mine; };
      if (was.mine && !mine && !own && this.o.status) this.o.status(who === "local" ? "Control taken at the radio: Take control to operate from here again" : who === "tci" ? "A TCI client took control: Take control to operate from here again" : who === "browser" ? "Control taken by " + name : "Control released", who !== "none", still);
      else if (took && mine && !was.mine && this.o.status) this.o.status("In control: this browser operates the radio", false, still);
      this.render(); if (this.o.onChange) this.o.onChange();
      return true;
    }
    if (head === "paired") {
      store(TOKEN_KEY, w[0]); this.pending = false; this.pairing = false; popClose();
      if (this.o.status) this.o.status("Paired as " + (w[1] === "admin" ? "the admin: this browser may operate and manage the others" : "an operator: this browser may operate the radio"));
      return true;
    }
    if (head === "away") { this.awayReady(w[0]); return true; }
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
      /* no new code yet: a few seconds after the last one, longer after wrong codes (the radio says how long) */
      else if (/^pairing paused/.test(body) && this.pairing) { this.pairError = "No code on the radio's screen now: " + body.replace(/^pairing paused: /, ""); this.pairOpen(); }
      else if (/^session revoked/.test(body)) { store(TOKEN_KEY, null); this.role = "view"; this.render(); }
      return false;    /* the page shows it too */
    }
    return false;
  };
  /* The radio's refusals about this browser's access or control hold as long as their cause (#58). One because the
   * radio is on the air (Take control, TUNE) holds while it is, and for its first second in any case: the radio tells
   * its TX state after the command it refused, so a command sent just as it keyed is refused before "tx 1" comes */
  const TX_TOLD_MS = 1000;
  Session.prototype.holds = function(err) {
    const self = this, at = Date.now();
    if (/^(view only|controlled by)/.test(err)) return function() { return !!self.can(); };
    if (/^(busy: the radio is transmitting|tune: on the air)/.test(err)) return function() { return self.radioTx || Date.now() - at < TX_TOLD_MS; };
    return null;
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
    /* while Funnel is on, for a paired browser at home (the page gives the Panel's address on the internet) */
    this.$away.prop("hidden", !(this.o.awayUrl && this.o.awayUrl()) || this.role === "view" || / \(away\)$/.test(this.name))
      .attr("title", "This browser on the internet, through Tailscale Funnel: a key of its own for the Panel away from home");
  };
  /* "Use away from home" (#58): the radio makes this browser a key of its own for the Panel on the internet, and the
   * page opens it there in this browser, the key after '#' in the link: a fragment goes to no server, and the
   * page there takes it out of the address bar. It never shows on the radio's screen, which any viewer may watch */
  Session.prototype.awayOpen = function() {
    const self = this, url = this.o.awayUrl && this.o.awayUrl();
    if (!url) return;
    popup("Use away from home", $("<div>").append(
      $("<p>").text("The radio's Panel is on the internet at " + url + " through Tailscale Funnel. Only a browser paired here gets in, with a key of its own."),
      $("<p>").text("This makes that key for this browser (\"" + this.name + " (away)\", with the same rights), and opens the Panel there in this browser: keep that page (a bookmark), it remembers the key. A new key replaces the one made before. Sessions lists it, and Revoke ends it, as any browser.")),
      [{t: "Make the key and open", primary: true, keep: true, cb: function() { self.o.send("away"); }}, {t: "Cancel"}]);
  };
  Session.prototype.awayReady = function(token) {
    const url = this.o.awayUrl && this.o.awayUrl();
    if (!url || !/^[0-9a-f]{32}$/.test(token)) return;
    const $a = $("<a class='btn primary' target='_blank' rel='noopener noreferrer'>").attr("href", url + "#t=" + token).text("Open the Panel away from home");
    popup("Use away from home", $("<div>").append(
      $("<p>").text("The key is made. Open the Panel on the internet in this browser, and keep that page: it remembers the key. The key is in the link, after '#', which no server sees, and the page takes it out of the address bar."),
      $("<p>").append($a)), [{t: "Close", primary: true}]);
    $a.on("click", function() { setTimeout(popClose, 0); });
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
    const self = this;
    if (this.ctl.mine) this.o.send("control release");
    else if (this.radioTx) { if (this.o.status) this.o.status("The radio is transmitting: control cannot move now", true, function() { return self.radioTx; }); }
    else { this.taking = true; this.o.send("control take"); }
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
        $("<span>").text(s.role + (s.online ? " · connected" : " · seen " + age(Math.max(0, Date.now() / 1000 - s.last))) + " · " + s.ip + (s.funnel ? " via Funnel" : "")),
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
    this.holds = null;    /* while a refusal's cause lasts (set) */
    const self = this;
    if (o.session) o.session.ptt = this;     /* its Mic on over is no takeover to the session (#97) */
    this.tick = setInterval(function() { if (self.state === "tx" || self.state === "refused") self.render(); }, 1000);
    this.render();
  }
  /* A transmission of the browser's microphone, from its press until the radio confirmed its end */
  Ptt.prototype.micOver = function() { return this.path === "mic" && (this.state === "requesting" || this.state === "tx" || this.state === "releasing"); };
  Ptt.prototype.clear = function() { if (this.timer) { clearTimeout(this.timer); this.timer = null; } if (this.renew) { clearInterval(this.renew); this.renew = null; } };
  Ptt.prototype.press = function() {
    const self = this, o = this.o;
    if (this.state === "requesting" || this.state === "tx") return;
    if (this.state === "releasing") { this.why = "Wait: the radio is still unkeying"; this.render(); return; }
    if (this.state === "busy") { this.set("busy", "The radio is transmitting already", function() { return self.state === "busy"; }); return; }
    if (o.link && o.link.stalled) { this.set("refused", "The radio does not answer: no transmission until it does", function() { return o.link.stalled; }); return; }
    const no = o.session ? o.session.can() : "";
    if (no) { this.set("refused", no, function() { return o.session.can() === no; }); return; }
    const mic = o.mic && o.mic.wanted();
    if (mic && !o.mic.ready()) { this.set("refused", "Microphone not ready: no transmission (the radio's own microphone is not used in its place)", function() { return o.mic.wanted() && !o.mic.ready(); }); return; }
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
      if (this.state === "requesting") { if (this.timer) { clearTimeout(this.timer); this.timer = null; } this.state = "tx"; this.why = ""; this.render(); if (this.o.status) this.o.status("On the air (" + (this.path === "mic" ? "browser microphone" : "the radio's microphone") + ")", false, function() { return self.state === "tx" || self.state === "releasing"; }); }
      else if (this.state === "idle" || this.state === "ended" || this.state === "refused") { this.state = "busy"; this.render(); }
    } else {
      if (this.state === "tx") { this.clear(); this.stopPath(); this.set("ended", "The radio ended the transmission"); }
      else if (this.state === "releasing" || this.state === "busy") this.set("idle", "");
    }
    if (!on && this.state === "ended" && !this.timer) this.timer = setTimeout(function() { if (self.state === "ended") self.set("idle", ""); }, ENDED_MS);
  };
  /* An err while a press waits for "tx 1": the PTT's own refusals (view only, control elsewhere, no TX for this
   * session, "ptt: no TX on ...": a frequency the radio may not transmit on, #121) end it, and let go as the 1.5 s
   * timeout does: an err cannot say which command it answers, and this press may have keyed. Another err (a tune
   * refused, a Take control refused while on the air) leaves it */
  Ptt.prototype.err = function(text) {
    if (/^ptt released/.test(text)) { this.clear(); if (this.renew) { clearInterval(this.renew); this.renew = null; } if (this.state === "tx" || this.state === "requesting") this.set("ended", "The radio unkeyed: " + text.replace(/^ptt released: /, "")); return true; }
    const ses = this.o.session;
    if (this.state === "requesting" && /^(controlled by|view only|this session may not transmit|ptt: )/.test(text)) { this.clear(); this.stopPath(); this.set("refused", text.replace(/^ptt: /, ""), ses && ses.holds ? ses.holds(text) : null); return true; }
    return false;
  };
  /* The setting changed (the microphone on or off): an old refusal or ending is no longer the news */
  Ptt.prototype.reset = function() { if (this.state === "refused" || this.state === "ended") this.set("idle", ""); else this.render(); };
  Ptt.prototype.lost = function(why) {
    if (this.state === "requesting" || this.state === "tx" || this.state === "releasing") { this.clear(); this.stopPath(); this.set("ended", why || "The link dropped: the transmission is over"); }
  };
  /* holds: how long the status line keeps why (status), and the button a refusal (render) */
  Ptt.prototype.set = function(state, why, holds) {
    const self = this;
    this.clear();
    this.state = state; this.why = why || ""; this.holds = holds || null;
    if (why && this.o.status) this.o.status(why, state !== "idle", holds);
    if (state === "ended" || state === "refused") this.timer = setTimeout(function() { if (self.state === state) self.set("idle", ""); }, ENDED_MS);
    this.render();
  };
  /* A refusal whose cause has gone (control back here, the radio answering again) goes with it, not 5 s later (#58) */
  Ptt.prototype.render = function() {
    if (this.state === "refused" && this.holds && !this.holds()) { this.set("idle", ""); return; }
    const el = this.o.el; if (!el) return;
    const mic = this.o.mic && this.o.mic.wanted(), tap = !!this.hands && this.hands.tap();   /* PTT options (#241) */
    let main, sub = "TX audio: " + (mic ? "browser microphone" : "microphone at the radio");
    switch (this.state) {
      case "requesting": main = "Keying..."; break;
      case "tx": { const s = Math.floor((Date.now() - this.since) / 1000); main = "ON AIR " + Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0"); sub = (tap ? "tap to stop · " : "") + (this.path === "mic" ? "browser microphone" : "microphone at the radio"); break; }
      case "releasing": main = "Unkeying..."; break;
      case "ended": main = "TX ended"; sub = "press again to transmit"; break;
      case "refused": main = "No TX"; sub = this.why.length > 48 ? this.why.slice(0, 46) + "…" : this.why; break;
      case "busy": main = "TX (not this browser)"; sub = "the radio is on the air"; break;
      default: main = tap ? "PTT (tap)" : "PTT (hold)";
    }
    el.classList.toggle("down", this.state === "tx" || this.state === "requesting");
    el.classList.toggle("warn", this.state === "ended" || this.state === "refused" || this.state === "busy");
    el.innerHTML = ""; el.appendChild(document.createTextNode(main)); const sm = document.createElement("small"); sm.textContent = sub; el.appendChild(sm);
  };

  /* ---- TUNE for an external tuner ---- */
  const TUNE_START_MS = 2000;
  function Tune(o) {
    this.o = o; this.state = "idle"; this.hold = 0; this.renew = null; this.timer = null; this.since = 0; this.limit = 0;
    this.why = ""; this.radioOn = false; this.holds = null;
    const self = this;
    this.tick = setInterval(function() { if (self.state === "on" || self.state === "refused" || (self.state === "idle" && self.radioOn)) self.render(); }, 1000);
    this.render();
  }
  Tune.prototype.clear = function() { if (this.timer) { clearTimeout(this.timer); this.timer = null; } if (this.renew) { clearInterval(this.renew); this.renew = null; } };
  Tune.prototype.press = function() {
    const self = this, o = this.o;
    if (this.state === "requesting" || this.state === "on" || this.radioOn) {
      this.clear(); o.send("tune 0");      /* a stop goes even to a stalled link: late, it still stops */
      this.set("stopping", "");
      this.timer = setTimeout(function() { if (self.state === "stopping") self.set("idle", ""); }, TUNE_START_MS);
      return;
    }
    if (o.link && o.link.stalled) { this.set("refused", "The radio does not answer: no tune until it does", function() { return o.link.stalled; }); return; }
    const no = o.session ? o.session.can() : "";
    if (no) { this.set("refused", no, function() { return o.session.can() === no; }); return; }
    const key = "tune 1 " + (++this.hold);
    this.clear();
    if (!o.send(key)) { this.set("refused", "No link to the radio: no tune"); return; }
    this.renew = setInterval(function() { o.send(key); }, RENEW_MS);
    this.state = "requesting"; this.why = ""; this.render();
    this.timer = setTimeout(function() {
      if (self.state !== "requesting") return;
      self.clear(); o.send("tune 0"); self.set("refused", "The radio did not start the tune (see its screen)");
    }, TUNE_START_MS);
  };
  Tune.prototype.radio = function(on, limit) {
    const self = this, was = this.radioOn;
    this.radioOn = !!on; if (limit) this.limit = limit;
    if (on && this.state === "requesting") {
      if (this.timer) { clearTimeout(this.timer); this.timer = null; }     /* the renewal goes on while it is on */
      this.state = "on"; this.since = Date.now(); this.why = "";
      if (this.o.status) this.o.status("TUNE on the air: press TUNE again to stop", false, function() { return self.state === "on" || self.state === "stopping"; });
    } else if (!on && this.state === "on") {
      this.clear(); this.set("ended", "The tune is over"); return;
    } else if (!on && this.state === "stopping") {
      this.clear(); this.set("idle", ""); return;
    } else if (on && !was && this.state !== "on") {
      this.since = Date.now();     /* started at the radio: this page only shows it, and may stop it */
    }
    this.render();
  };
  /* Its own: the refusals and stops that name the tune; while a press waits for the radio, the access refusals too */
  Tune.prototype.err = function(text) {
    if (!/^tune\b/.test(text) && !(this.state === "requesting" && /^(controlled by|view only|this session may not transmit)/.test(text))) return false;
    this.clear();
    const ses = this.o.session;
    if (/^tune stopped/.test(text)) { this.radioOn = false; this.set("ended", "The radio stopped the tune: " + text.replace(/^tune stopped: /, "")); }
    else this.set("refused", text.replace(/^tune: /, ""), ses && ses.holds ? ses.holds(text) : null);
    return true;
  };
  /* The stop goes behind the renewals a stalled link still holds, as the PTT's release does: delivered late, they end
   * with it, not with a tune kept three seconds past the last of them */
  Tune.prototype.lost = function(why) {
    this.radioOn = false;     /* not known any more: the radio stops it within 3 s, and its next state says */
    if (this.state === "requesting" || this.state === "on") { this.clear(); this.o.send("tune 0"); this.set("ended", why || "The link dropped: the radio stops the tune within 3 s"); }
    else if (this.state === "stopping") { this.clear(); this.set("idle", ""); }
  };
  Tune.prototype.set = function(state, why, holds) {
    const self = this;
    this.clear();
    this.state = state; this.why = why || ""; this.holds = holds || null;
    if (why && this.o.status) this.o.status(why, state === "ended" || state === "refused", holds);
    if (state === "ended" || state === "refused") this.timer = setTimeout(function() { if (self.state === state) self.set("idle", ""); }, ENDED_MS);
    this.render();
  };
  Tune.prototype.render = function() {
    if (this.state === "refused" && this.holds && !this.holds()) { this.set("idle", ""); return; }    /* as the PTT's */
    const el = this.o.el; if (!el) return;
    let main, sub = "external tuner";
    const s = Math.floor((Date.now() - this.since) / 1000), lim = this.limit ? " / " + this.limit + " s" : "";
    switch (this.state) {
      case "requesting": main = "Tuning..."; break;
      case "on": main = "TUNE " + s + lim; sub = "press to stop"; break;
      case "stopping": main = "Stopping..."; break;
      case "ended": main = "TUNE over"; sub = this.why.length > 40 ? this.why.slice(0, 38) + "…" : this.why; break;
      case "refused": main = "No tune"; sub = this.why.length > 40 ? this.why.slice(0, 38) + "…" : this.why; break;
      default: if (this.radioOn) { main = "TUNE " + s + lim; sub = "from the radio: press to stop"; } else main = "TUNE";
    }
    el.classList.toggle("down", this.state === "on" || this.state === "requesting" || (this.state === "idle" && this.radioOn));
    el.classList.toggle("warn", this.state === "ended" || this.state === "refused");
    el.innerHTML = ""; el.appendChild(document.createTextNode(main)); const sm = document.createElement("small"); sm.textContent = sub; el.appendChild(sm);
  };

  /* ---- PTT options (#241), as SVXConnect for Android has them, each off until chosen ---- */
  const HANDS_KEY = "x6100ptt";
  const HS = ["off", "play", "next", "prev", "held"];
  const HS_ACTIONS = ["play", "pause", "stop", "nexttrack", "previoustrack", "seekforward", "seekbackward"];
  /* Mute PTT: the level as SVXConnect's meter has it, the RMS as a bar of -60..0 dBFS, 15 times a second, 10 ms up and
   * 400 ms down; on the air at 0.05 for 80 ms, off at 0.02 for 300 ms. Armed by 300 ms of digital silence (under
   * -90 dBFS, which no open microphone gives) after some sound */
  const MUTE_TICK_MS = 67, MUTE_ON = 0.05, MUTE_OFF = 0.02, MUTE_ATTACK_MS = 80, MUTE_HANG_MS = 300, MUTE_ARM_MS = 300,
        MUTE_SILENT_RMS = 3.2e-5;
  function rmsBar(rms) {
    if (!(rms >= 1e-7)) return 0;
    return (Math.max(-60, Math.min(0, 20 * Math.log10(rms))) + 60) / 60;
  }
  /* 10 s of silence, 8 kHz 8 bit mono: what the page plays to be the phone's media player */
  function silentWav() {
    const n = 80000, b = new ArrayBuffer(44 + n), v = new DataView(b);
    const str = function(at, s) { for (let i = 0; i < s.length; i++) v.setUint8(at + i, s.charCodeAt(i)); };
    str(0, "RIFF"); v.setUint32(4, 36 + n, true); str(8, "WAVEfmt "); v.setUint32(16, 16, true); v.setUint16(20, 1, true);
    v.setUint16(22, 1, true); v.setUint32(24, 8000, true); v.setUint32(28, 8000, true); v.setUint16(32, 1, true);
    v.setUint16(34, 8, true); str(36, "data"); v.setUint32(40, n, true);
    new Uint8Array(b, 44).fill(128);
    return URL.createObjectURL(new Blob([b], {type: "audio/wav"}));
  }
  function Hands(o) {
    this.o = o; this.audio = null; this.w = null; this.line = "";
    this.opt = {tap: false, hs: "off", mute: false};
    try {
      const s = JSON.parse(load(HANDS_KEY) || "{}");
      this.opt.tap = s.tap === true; this.opt.mute = s.mute === true;
      if (HS.indexOf(s.hs) >= 0) this.opt.hs = s.hs;
    } catch (e) {}
    if (o.ptt) o.ptt.hands = this;    /* its button says hold or tap */
    const self = this, g = function() { self.gesture(); };
    document.addEventListener("pointerdown", g, true); document.addEventListener("keydown", g, true);
    this.timer = setInterval(function() { self.tick(); }, MUTE_TICK_MS);
    this.render(); this.media();
    if (o.ptt) o.ptt.render();
  }
  Hands.prototype.tap = function() { return this.opt.tap; };
  Hands.prototype.keyed = function() { const s = this.o.ptt.state; return s === "requesting" || s === "tx"; };
  Hands.prototype.toggle = function() { if (this.keyed()) this.o.ptt.release(); else this.o.ptt.press(); };
  Hands.prototype.save = function() { store(HANDS_KEY, JSON.stringify(this.opt)); };
  Hands.prototype.render = function() {
    const el = this.o.el, self = this; if (!el) return;
    el.innerHTML = '<details class="ptto"><summary>PTT options</summary>' +
      '<label>PTT button <select data-o="tap"><option value="0">Hold to talk</option><option value="1">Tap on, tap off</option></select></label>' +
      '<p class="sub">Tap on, tap off: a tap keys, the next one unkeys. The radio still ends a transmission after three minutes.</p>' +
      '<label>Bluetooth headset button <select data-o="hs"><option value="off">Off</option><option value="play">Play/pause (the middle button)</option>' +
      '<option value="next">Next track</option><option value="prev">Previous track</option><option value="held">Speaker mic, held to talk</option></select></label>' +
      '<p class="sub">The headset\'s button keys and unkeys while this page is on screen; a speaker mic that sends forward as its PTT goes down and backward as it comes up keys while held. ' +
      'For this the page plays silence as the phone\'s media player, and the headset\'s buttons come to it. A phone\'s volume keys, and the ' +
      'selfie remotes that send them, never reach a web page.</p>' +
      '<label><input type="checkbox" data-o="mute"> Headset mute as PTT</label>' +
      '<p class="sub">For a headset whose mute sends digital silence, as some do with the boom raised: with Mic on, the headset\'s sound ' +
      'keys the radio and its silence unkeys it. It arms once it has had that silence, so a phone\'s own microphone never keys it.</p>' +
      '<p class="ptto-st" aria-live="polite"></p></details>';
    el.querySelector('[data-o="tap"]').value = this.opt.tap ? "1" : "0";
    el.querySelector('[data-o="hs"]').value = this.opt.hs;
    el.querySelector('[data-o="mute"]').checked = this.opt.mute;
    el.querySelector('[data-o="tap"]').addEventListener("change", function() { self.opt.tap = this.value === "1"; self.save(); self.o.ptt.render(); });
    el.querySelector('[data-o="hs"]').addEventListener("change", function() { self.opt.hs = this.value; self.save(); self.media(); });
    el.querySelector('[data-o="mute"]').addEventListener("change", function() {
      self.opt.mute = this.checked; self.save();
      if (!self.opt.mute && self.w) { if (self.w.keyed && self.keyed()) self.o.ptt.release(); self.unwatch(); }
      self.tick();
    });
    this.show();
  };
  Hands.prototype.show = function() {
    const el = this.o.el && this.o.el.querySelector(".ptto-st"); if (!el) return;
    const o = this.o, w = this.w, parts = [];
    if (this.opt.hs !== "off") {
      if (!navigator.mediaSession) parts.push("Headset button: this browser gives a page no media buttons");
      else if (!this.audio || this.audio.paused) parts.push("Headset button: tap the page once to start");
      else parts.push("Headset button: ready");
    }
    if (this.opt.mute) {
      if (!o.mic.on() || !o.mic.stream()) parts.push("Headset mute: turn Mic on");
      else if (!w || (o.mic.ctx() && o.mic.ctx().state === "suspended")) parts.push("Headset mute: tap the page once to start");
      else if (!w.armed) parts.push("Headset mute: mute the headset once (raise the boom) to arm it");
      else parts.push("Headset mute: armed, unmute to transmit");
    }
    const t = parts.join(". ");
    if (t !== this.line) { this.line = t; el.textContent = t; }
  };
  /* The silent sound plays only after a tap or a key (browsers ask for one): tried again at each */
  Hands.prototype.gesture = function() {
    const ctx = this.opt.mute && this.o.mic.ctx();
    if (ctx && ctx.state === "suspended" && ctx.resume) ctx.resume().catch(function() {});
    this.play();
  };
  Hands.prototype.play = function() {
    const a = this.audio, self = this; if (!a || !a.paused) return;
    let p; try { p = a.play(); } catch (e) { p = null; }
    if (p && p.then) p.then(function() { if (self.audio === a) navigator.mediaSession.playbackState = "playing"; self.show(); }, function() { self.show(); });
  };
  Hands.prototype.media = function() {
    const ms = navigator.mediaSession, self = this, on = this.opt.hs !== "off";
    if (ms && on && !this.audio) {
      const a = document.createElement("audio");
      a.loop = true; a.setAttribute("playsinline", ""); a.src = silentWav();
      this.audio = a;
      HS_ACTIONS.forEach(function(n) { try { ms.setActionHandler(n, function() { self.action(n); }); } catch (e) {} });
      try { ms.metadata = new window.MediaMetadata({title: "PTT", artist: "OpenX6X00"}); } catch (e) {}
    } else if (ms && !on && this.audio) {
      const a = this.audio; this.audio = null;
      a.pause(); URL.revokeObjectURL(a.src); a.removeAttribute("src");
      HS_ACTIONS.forEach(function(n) { try { ms.setActionHandler(n, null); } catch (e) {} });
      try { ms.metadata = null; } catch (e) {}
      ms.playbackState = "none";
    }
    this.play(); this.show();
  };
  /* A headset button: the page stays the player whatever it was (a pause would hand the buttons to another app).
   * Out of sight nothing keys: the page lets PTT go then */
  Hands.prototype.action = function(n) {
    this.play();
    if (navigator.mediaSession) navigator.mediaSession.playbackState = "playing";
    if (document.hidden) return;
    const hs = this.opt.hs;
    if (hs === "held") {      /* a speaker mic's PTT: forward as it goes down, backward as it comes up */
      if (n === "seekforward" && !this.keyed()) this.o.ptt.press();
      else if (n === "seekbackward") this.o.ptt.release();
    } else if ((hs === "play" && (n === "play" || n === "pause")) || (hs === "next" && n === "nexttrack") ||
               (hs === "prev" && n === "previoustrack")) {
      this.toggle();
    }
  };
  /* The headset mute watch: the browser microphone's level while Mic is on */
  Hands.prototype.unwatch = function() {
    const w = this.w; this.w = null; if (!w) return;
    try { w.src.disconnect(); w.an.disconnect(); w.sink.disconnect(); } catch (e) {}
  };
  Hands.prototype.watch = function() {
    const o = this.o, s = this.opt.mute && o.mic.on() ? o.mic.stream() : null, ctx = s ? o.mic.ctx() : null;
    if (this.w && this.w.stream !== s) this.unwatch();
    if (!s || !ctx) return null;
    if (!this.w) {
      const src = ctx.createMediaStreamSource(s), an = ctx.createAnalyser(), sink = ctx.createGain();
      an.fftSize = 2048; sink.gain.value = 0;
      src.connect(an); an.connect(sink); sink.connect(ctx.destination);   /* pulled through a silent output */
      this.w = {stream: s, src: src, an: an, sink: sink, buf: new Float32Array(2048), level: 0, above: 0, below: 0,
                heard: false, silentSince: 0, armed: false, ready: false, keyed: false};
    }
    return ctx.state === "suspended" ? null : this.w;
  };
  Hands.prototype.tick = function(now) {
    const w = this.watch(), o = this.o;
    now = now || Date.now();
    if (w) {
      w.an.getFloatTimeDomainData(w.buf);
      let sum = 0;
      for (let i = 0; i < w.buf.length; i++) sum += w.buf[i] * w.buf[i];
      const rms = Math.sqrt(sum / w.buf.length), bar = rmsBar(rms);
      w.level += (bar - w.level) * (1 - Math.exp(-MUTE_TICK_MS / (bar > w.level ? 10 : 400)));
      const lvl = w.level < 0.005 ? 0 : w.level;
      if (rms >= MUTE_SILENT_RMS) { w.heard = true; w.silentSince = 0; }
      else if (!w.silentSince) w.silentSince = now;
      /* armed, and ready once the meter has fallen as at an unkey: its fall from the sound before is no new sound */
      if (!w.armed && w.heard && w.silentSince && now - w.silentSince >= MUTE_ARM_MS) {
        w.armed = true;
        if (o.status) o.status("Headset mute armed: unmute it to transmit");
      }
      if (lvl >= MUTE_ON) {
        w.below = 0; if (!w.above) w.above = now;
        if (w.armed && w.ready && !this.keyed() && !document.hidden && now - w.above >= MUTE_ATTACK_MS) {
          w.ready = false; w.keyed = true; o.ptt.press();   /* once: a refused or ended transmission waits for the mute again */
        }
      } else if (lvl <= MUTE_OFF) {
        w.above = 0; if (!w.below) w.below = now;
        if (now - w.below >= MUTE_HANG_MS) {
          if (w.keyed && this.keyed()) o.ptt.release();
          w.keyed = false; if (w.armed) w.ready = true;
        }
      }
    }
    this.show();
  };

  window.RadioSession = {create: function(o) { return new Session(o); }, forRadio: forRadio, takeLink: takeLink,
                         token: function() { return load(TOKEN_KEY); }};
  window.RadioPtt = {create: function(o) { return new Ptt(o); }};
  window.RadioPttHands = {create: function(o) { return new Hands(o); }};
  window.RadioTune = {create: function(o) { return new Tune(o); }};
})();
