/* A small DOM and jQuery stand-in, so that the browser pages' own scripts run under Node: enough of both
 * for edit_flow.js, radio_session.js and the pages' row and keyboard code, and no more. A selector or a
 * method it does not know throws, so a harness fails loudly rather than matching the wrong thing.
 * Selectors: tag, *, #id, .class, [attr], [attr=v], :not(simple), descendant and child combinators, lists. */
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const REPO = path.resolve(__dirname, '../../..');
/* X6_WEB: another copy of the web server's files, for instance an older revision's */
const WEB = process.env.X6_WEB || path.join(REPO, 'webserver/src/x6100_webserver');
const VOID = new Set(['input', 'br', 'img', 'hr', 'meta', 'link', 'use', 'path', 'circle', 'rect']);

/* ---- selectors ---- */
function splitTop(s, sep) {
  const out = []; let depth = 0, quote = null, cur = '';
  for (const c of s) {
    if (quote) { if (c === quote) quote = null; cur += c; continue; }
    if (c === '"' || c === "'") quote = c;
    else if (c === '[' || c === '(') depth++;
    else if (c === ']' || c === ')') depth--;
    if (c === sep && depth === 0) { out.push(cur); cur = ''; continue; }
    cur += c;
  }
  out.push(cur);
  return out.map(x => x.trim()).filter(x => x);
}
function parseCompound(s) {
  const tests = []; let rest = s.trim();
  while (rest) {
    let m;
    if ((m = /^\*/.exec(rest))) tests.push(() => true);
    else if ((m = /^([a-zA-Z][\w-]*)/.exec(rest))) { const t = m[1].toUpperCase(); tests.push(el => el.tagName === t); }
    else if ((m = /^#([\w-]+)/.exec(rest))) { const id = m[1]; tests.push(el => el.getAttribute('id') === id); }
    else if ((m = /^\.([\w-]+)/.exec(rest))) { const c = m[1]; tests.push(el => el.classList.contains(c)); }
    else if ((m = /^\[\s*([\w-]+)\s*(?:=\s*("[^"]*"|'[^']*'|[^\]\s]*))?\s*\]/.exec(rest))) {
      const name = m[1], v = m[2] === undefined ? null : m[2].replace(/^["']|["']$/g, '');
      tests.push(el => el.hasAttribute(name) && (v === null || el.getAttribute(name) === v));
    } else if ((m = /^:not\(/.exec(rest))) {
      let depth = 1, i = 5;
      for (; i < rest.length && depth; i++) { if (rest[i] === '(') depth++; else if (rest[i] === ')') depth--; }
      const inner = splitTop(rest.slice(5, i - 1), ',').map(parseCompound);
      tests.push(el => !inner.some(t => t(el)));
      rest = rest.slice(i); continue;
    } else throw new Error('browser-dom: selector not supported: ' + s);
    rest = rest.slice(m[0].length);
  }
  return el => el.nodeType === 1 && tests.every(t => t(el));
}
function parseComplex(s) {
  /* left to right: [{test, comb}], comb the combinator before the compound (' ' or '>') */
  const parts = []; let cur = '', comb = null, depth = 0, quote = null;
  const flush = () => { if (cur.trim()) { parts.push({test: parseCompound(cur), comb}); comb = ' '; } cur = ''; };
  for (const c of s) {
    if (quote) { if (c === quote) quote = null; cur += c; continue; }
    if (c === '"' || c === "'") { quote = c; cur += c; continue; }
    if (c === '[' || c === '(') depth++;
    if (c === ']' || c === ')') depth--;
    if (depth === 0 && (c === ' ' || c === '>')) { if (cur.trim()) flush(); if (c === '>') comb = '>'; continue; }
    cur += c;
  }
  flush();
  if (parts.length) parts[0].comb = null;
  return parts;
}
const selCache = new Map();
function compile(sel) {
  if (selCache.has(sel)) return selCache.get(sel);
  const complexes = splitTop(sel, ',').map(parseComplex);
  const matchAt = (el, parts, i) => {
    if (!parts[i].test(el)) return false;
    if (i === 0) return true;
    const comb = parts[i].comb;
    for (let a = el.parentNode; a && a.nodeType === 1; a = a.parentNode) {
      if (matchAt(a, parts, i - 1)) return true;
      if (comb === '>') return false;
    }
    return false;
  };
  const fn = el => el.nodeType === 1 && complexes.some(parts => matchAt(el, parts, parts.length - 1));
  selCache.set(sel, fn);
  return fn;
}

/* ---- nodes ---- */
class Event {
  constructor(type, init) {
    Object.assign(this, {bubbles: true, key: undefined, repeat: false, shiftKey: false, ctrlKey: false, altKey: false, metaKey: false, button: 0}, init || {});
    this.type = type; this.defaultPrevented = false; this.stopped = false; this.target = null; this.currentTarget = null;
  }
  preventDefault() { this.defaultPrevented = true; }
  stopPropagation() { this.stopped = true; }
  stopImmediatePropagation() { this.stopped = true; }
}
class Node {
  constructor(doc) { this.ownerDocument = doc; this.parentNode = null; this.childNodes = []; this.listeners = {}; }
  appendChild(n) { if (n.nodeType === 11) { [...n.childNodes].forEach(c => this.appendChild(c)); return n; } if (n.parentNode) n.parentNode.removeChild(n); n.parentNode = this; this.childNodes.push(n); return n; }
  insertBefore(n, ref) { if (!ref) return this.appendChild(n); if (n.parentNode) n.parentNode.removeChild(n); n.parentNode = this; this.childNodes.splice(this.childNodes.indexOf(ref), 0, n); return n; }
  removeChild(n) { const i = this.childNodes.indexOf(n); if (i >= 0) this.childNodes.splice(i, 1); n.parentNode = null; return n; }
  remove() { if (this.parentNode) this.parentNode.removeChild(this); }
  get children() { return this.childNodes.filter(c => c.nodeType === 1); }
  get textContent() { return this.childNodes.map(c => c.textContent).join(''); }
  set textContent(t) { this.childNodes.forEach(c => { c.parentNode = null; }); this.childNodes = []; if (t !== '' && t !== null && t !== undefined) this.appendChild(this.ownerDocument.createTextNode(String(t))); }
  addEventListener(type, fn) { (this.listeners[type] = this.listeners[type] || []).push(fn); }
  removeEventListener(type, fn) { const l = this.listeners[type] || []; const i = l.indexOf(fn); if (i >= 0) l.splice(i, 1); }
  dispatchEvent(ev) {
    ev.target = ev.target || this;
    const route = [];
    for (let n = this; n; n = n.parentNode) route.push(n);
    if (route[route.length - 1] === this.ownerDocument && this.ownerDocument.defaultView) route.push(this.ownerDocument.defaultView);
    for (const n of ev.bubbles ? route : [this]) {
      ev.currentTarget = n;
      for (const fn of [...(n.listeners[ev.type] || [])]) fn.call(n, ev);
      const jq = n.jq && n.jq[ev.type];
      if (jq) for (const fn of [...jq]) { if (fn.call(n, ev) === false) { ev.preventDefault(); ev.stopPropagation(); } }
      if (ev.stopped) break;
    }
    return !ev.defaultPrevented;
  }
  querySelectorAll(sel) { const test = compile(sel), out = []; const walk = n => n.children.forEach(c => { if (test(c)) out.push(c); walk(c); }); walk(this); return out; }
  querySelector(sel) { return this.querySelectorAll(sel)[0] || null; }
}
class Text extends Node {
  constructor(doc, t) { super(doc); this.nodeType = 3; this.data = t; }
  get textContent() { return this.data; }
  set textContent(t) { this.data = String(t); }
}
const BOOL = ['disabled', 'hidden', 'readonly', 'multiple', 'required'];
class Element extends Node {
  constructor(doc, tag) {
    super(doc); this.nodeType = 1; this.tagName = tag.toUpperCase(); this.attrs = new Map(); this.style = {};
    this._value = null; this._checked = null; this._selected = null;
  }
  getAttribute(n) { return this.attrs.has(n) ? this.attrs.get(n) : null; }
  setAttribute(n, v) { this.attrs.set(n, String(v)); }
  removeAttribute(n) { this.attrs.delete(n); }
  hasAttribute(n) { return this.attrs.has(n); }
  get id() { return this.getAttribute('id') || ''; }
  set id(v) { this.setAttribute('id', v); }
  get classList() {
    const el = this, get = () => (el.getAttribute('class') || '').split(/\s+/).filter(x => x), put = l => el.setAttribute('class', l.join(' '));
    return {contains: c => get().includes(c), add: (...cs) => put([...new Set([...get(), ...cs])]), remove: (...cs) => put(get().filter(x => !cs.includes(x))),
            toggle: (c, on) => { const has = get().includes(c), want = on === undefined ? !has : !!on; if (want !== has) (want ? put([...get(), c]) : put(get().filter(x => x !== c))); return want; }};
  }
  get dataset() {
    const el = this;
    return new Proxy({}, {get: (o, k) => el.getAttribute('data-' + String(k).replace(/[A-Z]/g, c => '-' + c.toLowerCase())) ?? undefined});
  }
  get type() {
    const t = (this.getAttribute('type') || '').toLowerCase();
    if (this.tagName === 'INPUT') return t || 'text';
    if (this.tagName === 'BUTTON') return t || 'submit';
    if (this.tagName === 'SELECT') return 'select-one';
    if (this.tagName === 'TEXTAREA') return 'textarea';
    return t;
  }
  get checked() { return this._checked === null ? this.hasAttribute('checked') : this._checked; }
  set checked(v) { this._checked = !!v; }
  get selected() { return this._selected === null ? this.hasAttribute('selected') : this._selected; }
  set selected(v) {
    this._selected = !!v;
    const sel = this.parentNode;
    if (v && sel && sel.tagName === 'SELECT') sel.querySelectorAll('option').forEach(o => { if (o !== this) o._selected = false; });
  }
  get value() {
    if (this.tagName === 'SELECT') { const opts = this.querySelectorAll('option'); const o = opts.find(x => x.selected) || opts[0]; return o ? o.value : ''; }
    if (this.tagName === 'OPTION') return this.hasAttribute('value') ? this.getAttribute('value') : this.textContent;
    if (this._value !== null) return this._value;
    if (this.tagName === 'TEXTAREA') return this.textContent;
    return this.getAttribute('value') || (this.type === 'checkbox' || this.type === 'radio' ? 'on' : '');
  }
  set value(v) {
    v = v === null || v === undefined ? '' : String(v);
    if (this.tagName === 'SELECT') { const o = this.querySelectorAll('option').find(x => x.value === v); this.querySelectorAll('option').forEach(x => { x._selected = x === o; }); return; }
    if (this.tagName === 'OPTION') { this.setAttribute('value', v); return; }
    this._value = v;
  }
  get maxLength() { return this.hasAttribute('maxlength') ? Number(this.getAttribute('maxlength')) : -1; }
  set maxLength(v) { this.setAttribute('maxlength', v); }
  get isContentEditable() {
    for (let n = this; n && n.nodeType === 1; n = n.parentNode) {
      const v = n.getAttribute('contenteditable');
      if (v === '' || v === 'true') return true;
      if (v === 'false') return false;
    }
    return false;
  }
  get focusable() {
    if (this.disabled) return false;
    return ['BUTTON', 'INPUT', 'SELECT', 'TEXTAREA'].includes(this.tagName) || (this.tagName === 'A' && this.hasAttribute('href')) || this.hasAttribute('tabindex') || this.isContentEditable;
  }
  focus() { if (this.focusable) { this.ownerDocument.activeElement = this; this.dispatchEvent(new Event('focus', {bubbles: false})); } }
  blur() { if (this.ownerDocument.activeElement === this) this.ownerDocument.activeElement = this.ownerDocument.body; }
  click() { if (this.disabled) return; this.dispatchEvent(new Event('click')); }
  matches(sel) { return compile(sel)(this); }
  closest(sel) { const test = compile(sel); for (let n = this; n && n.nodeType === 1; n = n.parentNode) if (test(n)) return n; return null; }
  set innerHTML(html) { this.textContent = ''; parseHTML(this.ownerDocument, html).forEach(n => this.appendChild(n)); }
  get innerHTML() { throw new Error('browser-dom: innerHTML is write-only here'); }
}
for (const name of BOOL) {
  Object.defineProperty(Element.prototype, name === 'readonly' ? 'readOnly' : name, {
    get() { return this.hasAttribute(name); },
    set(v) { if (v) this.setAttribute(name, ''); else this.removeAttribute(name); }
  });
}
class Fragment extends Node { constructor(doc) { super(doc); this.nodeType = 11; } }
class Document extends Node {
  constructor() {
    super(null); this.ownerDocument = this; this.nodeType = 9; this.defaultView = null;
    this.documentElement = this.createElement('html'); this.appendChild(this.documentElement);
    this.body = this.createElement('body'); this.documentElement.appendChild(this.body);
    this.activeElement = this.body;
  }
  createElement(t) { return new Element(this, t); }
  createTextNode(t) { return new Text(this, t); }
  createDocumentFragment() { return new Fragment(this); }
  getElementById(id) { return this.querySelector('#' + id); }
}
class Window extends Node { constructor(doc) { super(doc); this.nodeType = 0; } }

/* HTML as the pages and their scripts write it: tags with attributes, text, comments, a few entities */
function decode(t) { return t.replace(/&rarr;/g, '→').replace(/&gt;/g, '>').replace(/&lt;/g, '<').replace(/&nbsp;/g, ' ').replace(/&middot;/g, '·').replace(/&quot;/g, '"').replace(/&amp;/g, '&'); }
function parseHTML(doc, html) {
  const root = doc.createDocumentFragment(), stack = [root];
  const re = /<!--[\s\S]*?-->|<\/\s*([\w-]+)\s*>|<([\w-]+)((?:\s+[\w:-]+(?:\s*=\s*(?:"[^"]*"|'[^']*'|[^\s>"']+))?)*)\s*(\/?)>|([^<]+)/g;
  let m;
  while ((m = re.exec(html))) {
    const top = stack[stack.length - 1];
    if (m[0].startsWith('<!--')) continue;
    if (m[1]) { const t = m[1].toUpperCase(); for (let i = stack.length - 1; i > 0; i--) if (stack[i].tagName === t) { stack.length = i; break; } continue; }
    if (m[2]) {
      const el = doc.createElement(m[2]);
      const ar = /([\w:-]+)(?:\s*=\s*("[^"]*"|'[^']*'|[^\s>"']+))?/g; let a;
      while ((a = ar.exec(m[3] || ''))) el.setAttribute(a[1], a[2] === undefined ? '' : decode(a[2].replace(/^["']|["']$/g, '')));
      top.appendChild(el);
      if (!m[4] && !VOID.has(m[2].toLowerCase())) stack.push(el);
      continue;
    }
    if (m[5]) top.appendChild(doc.createTextNode(decode(m[5])));
  }
  return [...root.childNodes];
}

/* ---- jQuery, as far as the pages use it ---- */
function makeJQuery(doc, win) {
  const cache = new WeakMap();
  const dataOf = el => { let d = cache.get(el); if (!d) { d = {}; cache.set(el, d); } return d; };
  const clean = el => { if (el.nodeType !== 1) return; cache.delete(el); el.jq = null; el.children.forEach(clean); };
  /* jQuery's reading of a data-* attribute: "3" is the number 3, "{...}" JSON, the rest a string */
  const conv = v => { if (v === 'true') return true; if (v === 'false') return false; if (v === 'null') return null; if (v === String(+v)) return +v;
                      if (/^(?:\{[\w\W]*\}|\[[\w\W]*\])$/.test(v)) { try { return JSON.parse(v); } catch (e) {} } return v; };
  const nodesOf = c => c instanceof JQ ? [...c] : c && c.nodeType ? [c] : Array.isArray(c) ? c.flatMap(nodesOf)
                       : typeof c === 'string' || typeof c === 'number' ? parseHTML(doc, String(c)) : [];
  class JQ {
    constructor(list) { const seen = new Set(); let i = 0; for (const n of list) if (n && !seen.has(n)) { seen.add(n); this[i++] = n; } this.length = i; }
    *[Symbol.iterator]() { for (let i = 0; i < this.length; i++) yield this[i]; }
    get(i) { return i === undefined ? [...this] : this[i < 0 ? this.length + i : i]; }
    each(fn) { for (let i = 0; i < this.length; i++) if (fn.call(this[i], i, this[i]) === false) break; return this; }
    map(fn) { const out = []; this.each((i, el) => { const r = fn.call(el, i, el); if (r !== null && r !== undefined) out.push(...(Array.isArray(r) ? r : [r])); }); const q = new JQ([]); out.forEach((v, i) => { q[i] = v; }); q.length = out.length; return q; }
    filter(s) { return new JQ([...this].filter((el, i) => typeof s === 'function' ? s.call(el, i, el) : el.nodeType === 1 && el.matches(s))); }
    not(s) { return new JQ([...this].filter(el => !(el.nodeType === 1 && el.matches(s)))); }
    is(s) { return [...this].some(el => el.nodeType === 1 && el.matches(s)); }
    first() { return new JQ(this.length ? [this[0]] : []); }
    last() { return new JQ(this.length ? [this[this.length - 1]] : []); }
    eq(i) { return new JQ([this.get(i)].filter(x => x)); }
    find(s) { return new JQ([...this].flatMap(el => el.querySelectorAll(s))); }
    add(o) { return new JQ([...this, ...$(o)]); }
    children(s) { return new JQ([...this].flatMap(el => el.children.filter(c => !s || c.matches(s)))); }
    parent() { return new JQ([...this].map(el => el.parentNode).filter(n => n && n.nodeType === 1)); }
    closest(s) { return new JQ([...this].map(el => el.nodeType === 1 && el.closest(s)).filter(x => x)); }
    attr(n, v) {
      if (typeof n === 'object') { Object.keys(n).forEach(k => this.attr(k, n[k])); return this; }
      if (v === undefined) { const el = this[0]; return el && el.nodeType === 1 && el.hasAttribute(n) ? el.getAttribute(n) : undefined; }
      return this.each((i, el) => { if (v === null) el.removeAttribute(n); else el.setAttribute(n, v); });
    }
    removeAttr(n) { return this.each((i, el) => el.removeAttribute(n)); }
    prop(n, v) { if (v === undefined) return this[0] ? this[0][n] : undefined; return this.each((i, el) => { el[n] = v; }); }
    data(k, v) {
      if (typeof k === 'object') return this.each((i, el) => Object.assign(dataOf(el), k));
      if (v === undefined) {
        const el = this[0]; if (!el) return undefined;
        const d = dataOf(el);
        if (!(k in d)) { const a = el.getAttribute('data-' + k); if (a !== null) d[k] = conv(a); }
        return d[k];
      }
      return this.each((i, el) => { dataOf(el)[k] = v; });
    }
    removeData(k) { return this.each((i, el) => { delete dataOf(el)[k]; }); }
    val(v) { if (v === undefined) return this[0] ? this[0].value : undefined; return this.each((i, el) => { el.value = v; }); }
    text(v) { if (v === undefined) return [...this].map(el => el.textContent).join(''); return this.each((i, el) => { el.childNodes.forEach(clean); el.textContent = String(v); }); }
    html(v) { if (v === undefined) throw new Error('browser-dom: .html() is write-only here'); return this.each((i, el) => { el.childNodes.forEach(clean); el.innerHTML = String(v); }); }
    empty() { return this.each((i, el) => { el.childNodes.forEach(clean); el.textContent = ''; }); }
    append(...cs) { return this.each((i, el) => cs.forEach(c => nodesOf(c).forEach(n => el.appendChild(n)))); }
    prepend(...cs) { return this.each((i, el) => cs.slice().reverse().forEach(c => nodesOf(c).reverse().forEach(n => el.insertBefore(n, el.childNodes[0])))); }
    appendTo(t) { $(t).append(this); return this; }
    prependTo(t) { $(t).prepend(this); return this; }
    insertAfter(t) { const ref = $(t)[0]; if (ref) { const next = ref.parentNode.childNodes[ref.parentNode.childNodes.indexOf(ref) + 1]; [...this].forEach(n => ref.parentNode.insertBefore(n, next)); } return this; }
    remove() { return this.each((i, el) => { clean(el); el.remove(); }); }
    detach() { return this.each((i, el) => el.remove()); }
    on(types, fn) { return this.each((i, el) => types.split(/\s+/).forEach(t => { el.jq = el.jq || {}; (el.jq[t] = el.jq[t] || []).push(fn); })); }
    off() { return this.each((i, el) => { el.jq = null; }); }
    trigger(t) {
      return this.each((i, el) => {
        if (t === 'focus') { el.focus(); return; }
        /* jQuery runs its own handlers on a disabled button too; the browser's click on it does nothing */
        const ev = new Event(t); ev.target = el;
        for (let n = el; n; n = n.parentNode) {
          ev.currentTarget = n;
          if (!el.disabled) for (const fn of [...(n.listeners[t] || [])]) fn.call(n, ev);
          for (const fn of [...((n.jq && n.jq[t]) || [])]) fn.call(n, ev);
          if (ev.stopped) break;
        }
      });
    }
    focus() { return this.trigger('focus'); }
    toggleClass(c, on) { return this.each((i, el) => el.classList.toggle(c, typeof on === 'boolean' ? on : undefined)); }
    addClass(c) { return this.each((i, el) => c.split(/\s+/).filter(x => x).forEach(x => el.classList.add(x))); }
    removeClass(c) { return this.each((i, el) => c.split(/\s+/).filter(x => x).forEach(x => el.classList.remove(x))); }
    hasClass(c) { return [...this].some(el => el.classList.contains(c)); }
    css(k, v) { if (typeof k === 'object') return this.each((i, el) => Object.assign(el.style, k)); if (v === undefined) return this[0] && this[0].style[k]; return this.each((i, el) => { el.style[k] = v; }); }
    show() { return this.each((i, el) => { el.style.display = ''; }); }
    hide() { return this.each((i, el) => { el.style.display = 'none'; }); }
  }
  function $(s) {
    if (typeof s === 'function') { s(); return; }
    if (s instanceof JQ) return s;
    if (s === undefined || s === null) return new JQ([]);
    if (s.nodeType !== undefined || s === win) return new JQ([s]);
    if (Array.isArray(s)) return new JQ(s);
    if (typeof s === 'string' && s.trim().startsWith('<')) return new JQ(parseHTML(doc, s.trim()));
    return new JQ(doc.querySelectorAll(s));
  }
  /* The requests a page makes wait here until the harness answers them: ok(json), fail(status, why, json) */
  $.requests = [];
  $.ajax = function(o) {
    const req = {url: o.url, type: (o.type || 'GET').toUpperCase(), body: o.data === undefined ? undefined : JSON.parse(o.data), timeout: o.timeout,
      ok: json => { req.done = 'ok'; if (o.success) o.success(json); },
      fail: (status, why, json) => { req.done = 'fail'; if (o.error) o.error({status: status, responseJSON: json}, why || 'error'); }};
    $.requests.push(req);
    return req;
  };
  $.get = (url, data, cb) => $.ajax({url: url, type: 'GET', success: typeof data === 'function' ? data : cb});
  $.getJSON = (url, cb) => $.ajax({url: url, type: 'GET', success: cb});
  $.fn = JQ.prototype;
  return $;
}

/* bottle's "% for a, b in (('x', 'y'), ...):" ... "% end" over tuples of strings, as macros.html has it,
 * with {{a}} put in escaped */
function expand(text) {
  const lines = text.split('\n'), out = [];
  const esc = v => v.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
  for (let i = 0; i < lines.length; i++) {
    const m = /^%\s*for\s+([\w,\s]+?)\s+in\s+(.*)$/.exec(lines[i]);
    if (!m) { out.push(lines[i]); continue; }
    let head = m[2];
    while (!/:\s*$/.test(head)) head += ' ' + lines[++i].replace(/^\s*%/, '');
    const names = m[1].split(',').map(x => x.trim());
    const strings = [...head.matchAll(/'((?:[^'\\]|\\.)*)'|"((?:[^"\\]|\\.)*)"/g)].map(x => x[1] !== undefined ? x[1] : x[2]);
    const body = [];
    for (i++; i < lines.length && !/^\s*%\s*end\b/.test(lines[i]); i++) body.push(lines[i]);
    for (let k = 0; k + names.length <= strings.length; k += names.length)
      out.push(...body.map(l => l.replace(/\{\{(\w+)\}\}/g, (x, n) => names.includes(n) ? esc(strings[k + names.indexOf(n)]) : x)));
  }
  return out.join('\n');
}

/* A page: its markup in the body, base.html's helpers, jQuery; scripts run in it with page.run(file or text) */
function page(view) {
  const doc = new Document(), win = new Window(doc);
  doc.defaultView = win;
  const timers = [];
  const ctx = {document: doc, console, JSON, Math, Date, Object, Array, String, Number, Boolean, RegExp, Error, Promise, Set, Map, WeakMap, Proxy,
               parseInt, parseFloat, isNaN, encodeURIComponent, decodeURIComponent, URLSearchParams,
               setTimeout: (fn, ms) => { timers.push({fn, ms}); return timers.length; }, clearTimeout: () => {},
               setInterval: () => 0, clearInterval: () => {}, confirm: () => true, alert: () => {}, location: {protocol: 'http:', host: 'radio', hostname: 'radio', hash: '', pathname: '/'},
               navigator: {}, localStorage: {getItem: () => null, setItem: () => {}, removeItem: () => {}},
               addEventListener: (t, fn) => win.addEventListener(t, fn), removeEventListener: (t, fn) => win.removeEventListener(t, fn)};
  ctx.window = ctx;
  ctx.$ = ctx.jQuery = makeJQuery(doc, win);
  vm.createContext(ctx);
  const p = {doc, win, ctx, $: ctx.$, timers, Event,
    run: (fileOrText, name) => vm.runInContext(fileOrText.includes('\n') ? fileOrText : fs.readFileSync(path.join(WEB, fileOrText), 'utf8'), ctx, {filename: name || fileOrText}),
    eval: text => vm.runInContext(text, ctx),
    key: (type, key, target, more) => { const ev = new Event(type, Object.assign({key}, more || {})); (target || doc.activeElement || doc.body).dispatchEvent(ev); return ev; },
    type: (input, text) => { input.value = text; input.dispatchEvent(new Event('input')); },
    leaving: () => { const ev = new Event('beforeunload', {bubbles: false, returnValue: undefined}); win.dispatchEvent(ev); return ev.defaultPrevented; }};
  /* base.html's own script first: X6100_WAIT_MS, x6100FailText */
  const base = fs.readFileSync(path.join(WEB, 'views/base.html'), 'utf8');
  p.run(/<script>([\s\S]*?)<\/script>/.exec(base)[1], 'views/base.html');
  if (view) {
    const text = fs.readFileSync(path.join(WEB, 'views', view), 'utf8');
    const markup = expand(text).split('\n').filter(l => !/^\s*%/.test(l)).join('\n').replace(/<script[\s\S]*?<\/script>/g, '').replace(/<style[\s\S]*?<\/style>/g, '');
    parseHTML(doc, markup).forEach(n => doc.body.appendChild(n));
    p.script = [...text.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(m => m[1]).pop();
    p.scripts = [...text.matchAll(/<script src="\/static\/js\/([\w.-]+)\?v=\d+"><\/script>/g)].map(m => 'static/js/' + m[1]);
    p.source = text;
  }
  return p;
}

/* The text between two markers of a production file, the first marker included */
function between(text, from, to) {
  const a = text.indexOf(from);
  const b = text.indexOf(to, a + from.length);
  if (a < 0 || b < 0) throw new Error('browser-dom: markers not found: ' + from + ' ... ' + to);
  return text.slice(a, b);
}

module.exports = {page, between, Event, WEB};
