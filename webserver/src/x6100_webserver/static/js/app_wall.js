/* What the apps' pages (FT8, JS8) share: the app's waterfall, the map of the stations heard, the phone's tabs,
 * the wall's height, and the link to the radio. Needs jQuery, radio_link.js and radio_session.js. From OlivLoze's fork.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
const AppWall = (function() {
  "use strict";
  const MONO = "ui-monospace,SFMono-Regular,Menlo,Consolas,monospace";
  function pad(n) { return (n < 10 ? "0" : "") + n; }
  function hms(ms) { const d = new Date(ms); return pad(d.getUTCHours()) + ":" + pad(d.getUTCMinutes()) + ":" + pad(d.getUTCSeconds()); }

  /* A locator's middle, [lat, lon], null for none */
  function loc2ll(g) {
    g = (g || "").toUpperCase();
    if (!/^[A-R]{2}[0-9]{2}/.test(g)) return null;
    let lon = (g.charCodeAt(0) - 65) * 20 - 180 + (+g[2]) * 2, lat = (g.charCodeAt(1) - 65) * 10 - 90 + (+g[3]);
    if (/^[A-X]{2}$/.test(g.slice(4, 6))) { lon += (g.charCodeAt(4) - 65) / 12 + 1 / 24; lat += (g.charCodeAt(5) - 65) / 24 + 1 / 48; }
    else { lon += 1; lat += 0.5; }
    return [lat, lon];
  }

  /* The ham band of a frequency (Hz), "" outside them: for the page's top line only */
  function band(hz) {
    const B = [[1800, 2000, "160 m"], [3500, 4000, "80 m"], [5300, 5410, "60 m"], [7000, 7300, "40 m"], [10100, 10150, "30 m"], [14000, 14350, "20 m"],
               [18068, 18168, "17 m"], [21000, 21450, "15 m"], [24890, 24990, "12 m"], [28000, 29700, "10 m"], [50000, 54000, "6 m"]];
    const k = hz / 1000, b = B.find(function(x) { return k >= x[0] && k <= x[1]; });
    return b ? b[2] : "";
  }

  /* ---- Countries and flags: cty.dat as the radio reads it (/api/dxcc, looked up as dxcc_lookup in src/dxcc.c
   * does), the radio's flags (src/flags.h's list, drawn larger in flags.png by tools/flags_web). Loaded once;
   * ready(fn) runs fn when they are */
  const cty = {t: null, f: null, fns: [], cache: {}};
  if (window.fetch) Promise.all([fetch("/api/dxcc").then(function(r) { return r.json(); }),
                                 fetch("/static/img/flags.json?v=1").then(function(r) { return r.json(); })]).then(function(v) {
    cty.t = v[0]; cty.f = v[1];
    cty.fns.forEach(function(fn) { fn(); });
  }).catch(function() {});
  function ready(fn) { if (cty.t) fn(); else cty.fns.push(fn); }
  /* is_modifier there: how or where a station operates, not its country */
  const MODS = {P: 1, M: 1, A: 1, B: 1, R: 1, T: 1, J: 1, QRP: 1, QRPP: 1, LH: 1, LGT: 1, AE: 1, AG: 1, KT: 1};
  function has(o, k) { return Object.prototype.hasOwnProperty.call(o, k); }
  function longest(s) { for (let n = s.length; n > 0; n--) if (has(cty.t.p, s.slice(0, n))) return cty.t.p[s.slice(0, n)]; return -1; }
  function byCall(s) { return has(cty.t.x, s) ? cty.t.x[s] : longest(s); }
  function lookup(s) {
    if (s.length < 2 || s.indexOf("...") >= 0) return -1;     /* "<...>": a hash not resolved */
    if (has(cty.t.x, s)) return cty.t.x[s];                   /* whole calls may have a '/' themselves */
    const parts = [];
    for (const p of s.split("/")) {
      if (!p) continue;
      if (parts.length >= 3) break;
      if (p === "MM" || p === "AM") return -1;                /* maritime or aeronautical mobile */
      if (parts.length && (has(MODS, p) || /^\d$/.test(p))) continue;
      parts.push(p);
    }
    if (parts.length === 1) return byCall(parts[0]);
    if (parts.length < 2) return -1;
    const sh = parts[1].length < parts[0].length ? 1 : 0;     /* "DL/SP2KMO" or "SP2KMO/DL": the shorter says where */
    const e = longest(parts[sh]);
    return e >= 0 ? e : byCall(parts[1 - sh]);
  }
  /* {k: "Italy", pfx: "I", ll: [lat, lon] of the country's centre, cell: its flag in flags.png or -1}, null unknown */
  function country(call) {
    if (!cty.t || !call) return null;
    const s = String(call).toUpperCase().replace(/[<>\s]/g, "").slice(0, 23);     /* CALL_MAX there */
    if (has(cty.cache, s)) return cty.cache[s];
    const e = lookup(s), en = e >= 0 ? cty.t.e[e] : null;
    return cty.cache[s] = en ? {k: en[0], pfx: en[1], ll: [en[2], en[3]], cell: has(cty.f.of, en[1]) ? cty.f.of[en[1]] : -1} : null;
  }
  /* The flag of a call's country, as an element (null when unknown); 20 x 15 here, 40 x 30 in flags.png */
  function flag(call) {
    const c = country(call);
    if (!c || c.cell < 0) return null;
    const n = cty.f.n, el = document.createElement("span");
    el.className = "flag"; el.title = c.k;
    el.style.backgroundPosition = (-(c.cell % n) * 20) + "px " + (-Math.floor(c.cell / n) * 15) + "px";
    return el;
  }
  /* Kilometres between two [lat, lon] */
  function km(a, b) {
    if (!a || !b) return 0;
    const r = Math.PI / 180, d = Math.pow(Math.sin((b[0] - a[0]) * r / 2), 2) + Math.cos(a[0] * r) * Math.cos(b[0] * r) * Math.pow(Math.sin((b[1] - a[1]) * r / 2), 2);
    return Math.round(12742 * Math.asin(Math.sqrt(d)));
  }
  /* How far a station is from me: from its locator, else from its country's centre ("≈") */
  function distance(me, call, grid) {
    const g = loc2ll(grid);
    if (g) { const d = km(me, g); return d ? d.toLocaleString("en").replace(/,/g, " ") + " km" : ""; }
    const c = country(call), d = c ? km(me, c.ll) : 0;
    return d ? "≈ " + d.toLocaleString("en").replace(/,/g, " ") + " km" : "";
  }

  /* ---- The app's waterfall: binary rows {u16 0xFFFD, u16 bins, u16 low Hz, u16 high Hz} then int8 dB over the
   * noise floor, the newest on top. o.now(): the radio's clock (ms) a row is stamped with; o.draw(ctx, W, H, wf):
   * the page's marks over it; o.click(hz) */
  function waterfall(o) {
    const box = document.getElementById("wfb"), cv = document.getElementById("wfc"), ctx = cv.getContext("2d");
    const over = document.getElementById("wfo"), octx = over.getContext("2d"), empty = document.getElementById("wf-empty");
    const ROWS = cv.height, palette = new Uint8Array(256 * 3);
    const stops = [[0, 6, 7, 10], [0.3, 14, 34, 110], [0.55, 30, 160, 220], [0.8, 255, 216, 96], [1, 255, 255, 255]];
    for (let i = 0; i < 256; i++) {
      const t = i / 255; let k = 0;
      while (k < stops.length - 2 && t > stops[k + 1][0]) k++;
      const a = stops[k], b = stops[k + 1], u = (t - a[0]) / (b[0] - a[0]);
      for (let c = 0; c < 3; c++) palette[i * 3 + c] = Math.round(a[c + 1] + (b[c + 1] - a[c + 1]) * u);
    }
    const wf = {n: 0, lo: 0, hi: 3000, rowT: [], last: 0, rows: ROWS};
    ctx.fillStyle = "#000"; ctx.fillRect(0, 0, cv.width, ROWS);
    wf.hzX = function(hz, W) { return (hz - wf.lo) * W / (wf.hi - wf.lo); };
    function scale() {
      const $s = $("#scale").empty();
      for (let hz = Math.ceil(wf.lo / 250) * 250; hz <= wf.hi; hz += 250) {
        const p = (hz - wf.lo) * 100 / (wf.hi - wf.lo);
        $s.append($("<i>").css("left", p + "%"));
        if (hz % 500 === 0 && p > 3 && p < 97) $s.append($("<span>").css("left", p + "%").text(hz));
      }
    }
    scale();
    wf.row = function(buf) {
      if (buf.byteLength < 8 || new DataView(buf).getUint16(0, true) !== 0xFFFD) return false;   /* the Panel's spectrum */
      const dv = new DataView(buf), n = dv.getUint16(2, true), lo = dv.getUint16(4, true), hi = dv.getUint16(6, true);
      if (!n || buf.byteLength < 8 + n || hi <= lo) return false;
      if (n !== wf.n || lo !== wf.lo || hi !== wf.hi) {
        wf.n = n; wf.lo = lo; wf.hi = hi; cv.width = n; wf.rowT = [];
        ctx.fillStyle = "#000"; ctx.fillRect(0, 0, n, ROWS);
        scale();
      }
      const db = new Int8Array(buf, 8, n);
      ctx.drawImage(cv, 0, 1);
      const img = ctx.createImageData(n, 1), d = img.data;
      for (let i = 0; i < n; i++) {
        const v = Math.max(0, Math.min(255, Math.round(db[i] * 255 / 30)));   /* 0..30 dB over the floor, as the radio */
        d[i * 4] = palette[v * 3]; d[i * 4 + 1] = palette[v * 3 + 1]; d[i * 4 + 2] = palette[v * 3 + 2]; d[i * 4 + 3] = 255;
      }
      ctx.putImageData(img, 0, 0);
      wf.rowT.unshift(o.now());
      if (wf.rowT.length > ROWS) wf.rowT.length = ROWS;
      wf.last = Date.now();
      empty.hidden = true;
      wf.redraw();
      return true;
    };
    wf.redraw = function() {
      const dpr = window.devicePixelRatio || 1, W = box.clientWidth, H = box.clientHeight;
      if (over.width !== Math.round(W * dpr) || over.height !== Math.round(H * dpr)) { over.width = Math.round(W * dpr); over.height = Math.round(H * dpr); }
      octx.setTransform(dpr, 0, 0, dpr, 0, 0);
      octx.clearRect(0, 0, W, H);
      if (wf.n && W) o.draw(octx, W, H, wf);
    };
    /* The row drawn nearest a time (ms on the radio's clock), -1 off the picture */
    wf.rowAt = function(ms) {
      const t = wf.rowT;
      if (!t.length || ms > t[0] + 2000 || ms < t[t.length - 1]) return -1;
      let i = 0; while (i + 1 < t.length && t[i + 1] >= ms) i++;
      return i;
    };
    /* A band over the whole height: our TX (red) or the station worked, its name at the bottom */
    wf.mark = function(c, W, H, hz, width, col, fill, text, row) {
      const x0 = wf.hzX(hz, W), x1 = Math.max(x0 + 2, wf.hzX(hz + width, W));
      c.fillStyle = fill; c.fillRect(x0, 0, x1 - x0, H);
      c.strokeStyle = col; c.beginPath(); c.moveTo(x0 + .5, 0); c.lineTo(x0 + .5, H); c.moveTo(x1 - .5, 0); c.lineTo(x1 - .5, H); c.stroke();
      c.font = "600 11px " + MONO; c.textBaseline = "top";
      const tw = c.measureText(text).width + 10, lx = Math.max(0, Math.min(W - tw, x0)), y = H - 16 - (row || 0) * 17;
      c.fillStyle = col; c.fillRect(lx, y, tw, 16);
      c.fillStyle = "#111"; c.fillText(text, lx + 5, y + 2);
    };
    /* Calls at their offset, in up to three lanes from y down, none past yMax */
    wf.labels = function(c, W, list, y, yMax, width) {
      c.font = "600 11px " + MONO; c.textBaseline = "top";
      const lanes = [];
      list.forEach(function(l) {
        const tw = c.measureText(l.c).width + 6, x = Math.max(0, Math.min(W - tw, wf.hzX(l.f + (width || 50) / 2, W) - tw / 2));
        let k = 0; while (lanes[k] && lanes[k].some(function(r) { return x < r[1] + 3 && x + tw > r[0] - 3; })) k++;
        if (k > 2) return;
        (lanes[k] = lanes[k] || []).push([x, x + tw]);
        const ly = y + 3 + k * 16; if (ly + 14 > yMax) return;
        c.fillStyle = "rgba(11,19,32,.82)"; c.fillRect(x, ly, tw, 14);
        c.fillStyle = l.col; c.fillText(l.c, x + 3, ly + 1);
      });
    };
    wf.closed = function(text) { empty.hidden = false; empty.textContent = text; };
    box.addEventListener("click", function(e) {
      if (!wf.n) return;
      const r = box.getBoundingClientRect();
      o.click(wf.lo + (e.clientX - r.left) * (wf.hi - wf.lo) / r.width);
    });
    window.addEventListener("resize", wf.redraw);
    return wf;
  }

  /* ---- The map: the land (or NASA's Blue Marble), us, the stations heard at their locator, a path to the one
   * worked. o: {me: [lat, lon], meCall, key (this browser's choices), colors: {cls: colour}, focus(): the call
   * worked, count(n)}.
   * map.stations[call] = {c, g, ll, cls, t (ms), k, d, snr} */
  function map(o) {
    const svgNS = "http://www.w3.org/2000/svg", svg = document.getElementById("map"), m = {stations: {}, hl: ""};
    fetch("/static/img/world.svg?v=1").then(function(r) { return r.text(); }).then(function(t) {
      const d = /\sd="([^"]+)"/.exec(t); if (d) document.getElementById("land").setAttribute("d", d[1]);
    }).catch(function() {});
    function xy(ll) { return [ll[1] + 180, 90 - ll[0]]; }
    function setView(v) {
      $("#z-near").attr("aria-pressed", String(v === "near")); $("#z-world").attr("aria-pressed", String(v === "world"));
      const c = o.me ? xy(o.me) : [182, 42];
      svg.setAttribute("viewBox", v === "world" ? "0 8 360 160" : (c[0] - 38) + " " + (c[1] - 21) + " 76 44");
      m.draw();
    }
    $("#z-near").on("click", function() { setView("near"); });
    $("#z-world").on("click", function() { setView("world"); });
    /* Satellite: NASA's Blue Marble (public domain, equirectangular as the map, 2560 x 1280: 280 kB on the
     * radio), loaded the first time it is asked for; on by default on a computer, off on a phone, where it is
     * the most of the page's weight; this browser keeps the choice */
    function setSat(on) {
      const img = document.getElementById("sat");
      if (on && !img.getAttribute("href")) img.setAttribute("href", "/static/img/world_sat.jpg?v=1");
      img.setAttribute("display", on ? "inline" : "none");
      svg.classList.toggle("sat", on);
      $("#z-sat").attr("aria-pressed", String(on));
      try { localStorage.setItem(o.key + ".sat", on ? "1" : "0"); } catch (e) {}
    }
    $("#z-sat").on("click", function() { setSat($(this).attr("aria-pressed") !== "true"); });
    /* The great circle from a to b, broken where it crosses the edge of the map */
    function greatCircle(a, b) {
      const r = Math.PI / 180;
      const v = function(p) { return [Math.cos(p[0] * r) * Math.cos(p[1] * r), Math.cos(p[0] * r) * Math.sin(p[1] * r), Math.sin(p[0] * r)]; };
      const A = v(a), B = v(b), w = Math.acos(Math.max(-1, Math.min(1, A[0] * B[0] + A[1] * B[1] + A[2] * B[2])));
      let d = "", last = null;
      for (let i = 0; i <= 64; i++) {
        const t = i / 64, s1 = w ? Math.sin((1 - t) * w) / Math.sin(w) : 1 - t, s2 = w ? Math.sin(t * w) / Math.sin(w) : t;
        const x = s1 * A[0] + s2 * B[0], y = s1 * A[1] + s2 * B[1], z = s1 * A[2] + s2 * B[2];
        const p = xy([Math.atan2(z, Math.hypot(x, y)) / r, Math.atan2(y, x) / r]);
        d += (last === null || Math.abs(p[0] - last) > 180 ? "M" : "L") + p[0].toFixed(2) + " " + p[1].toFixed(2);
        last = p[0];
      }
      return d;
    }
    function el(name, attrs, parent) {
      const e = document.createElementNS(svgNS, name);
      Object.keys(attrs).forEach(function(k) { e.setAttribute(k, attrs[k]); });
      parent.appendChild(e);
      return e;
    }
    function label(g, p, text, col, u) {
      el("text", {x: p[0] + 2.2 * u, y: p[1] + 1.2 * u, fill: col, "font-size": 3.6 * u, "font-family": MONO,
                  "font-weight": 600, "paint-order": "stroke", stroke: "#0b1320", "stroke-width": 0.8 * u}, g).textContent = text;
    }
    m.draw = function() {
      const g = document.getElementById("dots"), gc = document.getElementById("gc");
      g.textContent = ""; gc.textContent = "";
      const u = svg.viewBox.baseVal.width / 160, now = Date.now(), focus = o.focus(), target = m.stations[focus];
      let n = 0;
      if (o.me && target && target.ll)
        el("path", {d: greatCircle(o.me, target.ll), fill: "none", stroke: o.colors.mine, "stroke-width": 1.6, "stroke-dasharray": "5 4",
                    "vector-effect": "non-scaling-stroke"}, gc);
      Object.values(m.stations).sort(function(a, b) { return a.t - b.t; }).forEach(function(s) {
        const age = (now - s.t) / 60000;
        if (age > 30) { delete m.stations[s.c]; return; }    /* not heard for half an hour */
        n++;
        const p = xy(s.ll), big = s.c === focus || s.c === m.hl || s.cls === "me", col = o.colors[s.cls] || o.colors.oth;
        const c = el("circle", {cx: p[0], cy: p[1], r: (big ? 1.5 : 1) * u, fill: col, "fill-opacity": Math.max(0.25, 1 - age / 15).toFixed(2),
                                stroke: "#0b1320", "stroke-width": 0.8, "vector-effect": "non-scaling-stroke"}, g);
        el("title", {}, c).textContent = s.c + " " + s.g + (s.k ? " · " + s.k : "") + (s.d ? " · " + s.d + " km" : "");
        if (big) {
          el("circle", {cx: p[0], cy: p[1], r: 1.5 * u, fill: "none", stroke: col, "stroke-width": 1.2, "vector-effect": "non-scaling-stroke", class: "pulse"}, g);
          label(g, p, s.c, col, u);
        }
      });
      if (o.me) {
        const p = xy(o.me);
        el("rect", {x: p[0] - 1.3 * u, y: p[1] - 1.3 * u, width: 2.6 * u, height: 2.6 * u, fill: o.colors.mine, transform: "rotate(45 " + p[0] + " " + p[1] + ")"}, g);
        label(g, p, o.meCall || "ME", o.colors.mine, u);
      }
      o.count(n);
    };
    let timer = 0;
    m.soon = function() { if (!timer) timer = setTimeout(function() { timer = 0; m.draw(); }, 300); };
    /* A station heard: placed at its locator, or only freshened when it gave none */
    m.heard = function(call, grid, cls, extra) {
      if (!call) return;
      const ll = loc2ll(grid), old = m.stations[call];
      if (ll) m.stations[call] = Object.assign({c: call, g: grid, ll: ll}, extra || {}, {cls: cls, t: Date.now()});
      else if (old) Object.assign(old, extra || {}, {cls: cls, t: Date.now()});
      m.soon();
    };
    let sat = null; try { sat = localStorage.getItem(o.key + ".sat"); } catch (e) {}
    setSat(sat === null ? window.innerWidth > 760 : sat === "1");
    setView("near");
    setInterval(m.draw, 30000);
    return m;
  }

  /* ---- The phone's tabs: the waterfall or the map on top, one of the two lists under them; this browser keeps
   * the choice. onShow(id): a screen just shown, to draw (a hidden one has no size) */
  function tabs(key, onShow) {
    function show(id) {
      const g = $("#" + id).attr("data-g");
      $('.tabs[data-g="' + g + '"] button').each(function() { this.setAttribute("aria-selected", String(this.dataset.t === id)); });
      $('.pane[data-g="' + g + '"]').removeClass("show"); $("#" + id).addClass("show");
      onShow(id);
      try { localStorage.setItem(key + ".tab." + g, id); } catch (e) {}
    }
    $(".tabs").on("click", "button", function() { show(this.dataset.t); });
    ["top", "bot"].forEach(function(g) {
      let id = null; try { id = localStorage.getItem(key + ".tab." + g); } catch (e) {}
      if (id && $('.pane[data-g="' + g + '"]#' + id).length) show(id);
    });
  }

  /* ---- The wall fills the window under the page's top, the desk under it (a computer) */
  function fit() {
    const wall = document.getElementById("wall");
    if (window.innerWidth <= 760) { wall.style.height = ""; return; }
    const top = wall.getBoundingClientRect().top + window.scrollY, desk = document.getElementById("desk").offsetHeight;
    wall.style.height = Math.max(480, window.innerHeight - top - desk - 22) + "px";
  }
  window.addEventListener("resize", fit);

  /* ---- The link to the radio, the Panel's (pairing, control, health chips): o.hello (what to ask for once
   * connected), o.onText(head, body), o.onBinary(buf), o.onOpen(). Returns {send(s), act(s), status(text, error),
   * can(), shown()}: act is for what changes the radio, sent only from a paired browser in control, as on the Panel */
  function connect(o) {
    const secure = location.protocol === "https:", $st = $("#st");
    let link = null, ses = null, conf = null, holds = null;
    /* holds: while the cause of a refusal lasts (ses.holds); the line is cleared after it, as on the Panel */
    function status(text, error, h) { holds = h || null; $st.toggleClass("error", !!error).text(text || ""); }
    setInterval(function() { if (holds && !holds()) { holds = null; $st.removeClass("error").empty(); } }, 500);
    function send(s) { return !!link && link.send(s); }
    function act(s) {
      if (!link || !link.up) { status("The radio is not connected", true); return false; }
      if (link.stalled) { status("The radio does not answer: nothing is sent until it does", true); return false; }
      const why = ses.can();
      if (why) { status(why, true); ses.explain(); return false; }
      return send(s);
    }
    function health() { RadioLink.health(document.getElementById("health"), ses.chips()); }
    function wsUrl() { return secure ? "wss://" + location.host + "/ws/remote" : "ws://" + location.hostname + ":" + conf.port + "/"; }
    link = RadioLink.create({
      url: function() { return $.get("/api/remote").then(function(c) { conf = c; return c.enabled ? wsUrl() : null; }, function() { return null; }); },
      ping: "ping",
      onOpen: function() { o.onOpen(); ses.onOpen(); send("screen 0"); send("panel 1"); o.hello.forEach(send); status("Connected"); },
      onText: function(t) {
        const sp = t.indexOf(" "), head = sp < 0 ? t : t.slice(0, sp), body = sp < 0 ? "" : t.slice(sp + 1);
        try {
          if (ses.onText(head, body)) { health(); return; }
          if (head === "err") status(body, true, ses.holds(body));
          else o.onText(head, body);
        } catch (e) { status("Bad message: " + e, true); }
      },
      onBinary: o.onBinary,
      onClose: function() { status(conf && !conf.enabled ? "The browser remote is off on the radio: the Panel page turns it on" : "Not connected: trying again...", true); },
      onStall: function(on) { if (on) status("The radio does not answer", true); else status("Connected"); },
    });
    ses = RadioSession.create({send: send, status: status, el: document.getElementById("ses"), onChange: health});
    link.connect();
    return {send: send, act: act, status: status, can: function() { return ses ? ses.can() : "not connected"; },
            shown: function() { return $st.text(); }};
  }

  return {pad: pad, hms: hms, loc2ll: loc2ll, band: band, ready: ready, country: country, flag: flag, km: km, distance: distance,
          waterfall: waterfall, map: map, tabs: tabs, fit: fit, connect: connect, MONO: MONO};
})();
