#include "card_js.h"

#include <chrono>
#include <cstring>

#include "dom.h"
#include "nlohmann/json.hpp"

extern "C" {
#include "quickjs.h"
}

namespace anki::js {

namespace {

// The browser-like API scripts see, built on a few native DOM primitives (__n).
const char* PRELUDE = R"JS(
(function(){
const N = __n;
const cache = new Map();
const listeners = new Map();
const NODE_TYPES = [1, 3, 8, 9];
function wrap(id){ if (id === null || id === undefined || id < 0) return null; let w = cache.get(id); if (w) return w;
  const t = N.type(id); w = t === 0 ? new HTMLElement(id) : t === 3 ? document : new CharacterData(id); cache.set(id, w); return w; }
function ids(list){ return list.map(wrap); }
function kebab(p){ return p.replace(/[A-Z]/g, m => '-' + m.toLowerCase()); }
function parseStyle(s){ const m = new Map(); (s||'').split(';').forEach(d => { const i = d.indexOf(':'); if (i > 0) m.set(d.slice(0,i).trim().toLowerCase(), d.slice(i+1).trim()); }); return m; }
function writeStyle(id, m){ const s = [...m].filter(([k,v]) => v !== '' && v != null).map(([k,v]) => k + ': ' + v).join('; '); if (s) N.setAttr(id, 'style', s); else N.removeAttr(id, 'style'); }

class Event { constructor(type, init){ init = init || {}; this.type = type; this.bubbles = init.bubbles !== false; this.defaultPrevented = false; this._stop = false; Object.assign(this, init); }
  preventDefault(){ this.defaultPrevented = true; } stopPropagation(){ this._stop = true; } stopImmediatePropagation(){ this._stop = true; } }
class KeyboardEvent extends Event {} class MouseEvent extends Event {} class CustomEvent extends Event { constructor(t, i){ super(t, i); this.detail = (i||{}).detail; } }

class EventTargetBase {
  addEventListener(t, f){ if (typeof f !== 'function' && !(f && f.handleEvent)) return; const k = this._key(); if (!listeners.has(k)) listeners.set(k, {}); const m = listeners.get(k); (m[t] = m[t] || []).push(f); }
  removeEventListener(t, f){ const m = listeners.get(this._key()); if (m && m[t]) m[t] = m[t].filter(x => x !== f); }
  _fire(e){ const m = listeners.get(this._key()); e.currentTarget = this;
    if (m && m[e.type]) for (const f of m[e.type].slice()) { try { typeof f === 'function' ? f.call(this, e) : f.handleEvent(e); } catch (err) { console.error(String(err)); } }
    const h = this['on' + e.type]; if (typeof h === 'function') { try { h.call(this, e); } catch (err) { console.error(String(err)); } }
    if (this._id !== undefined && N.type(this._id) === 0) { const code = N.attr(this._id, 'on' + e.type); if (code) { try { (new Function('event', code)).call(this, e); } catch (err) { console.error(String(err)); } } } }
  dispatchEvent(e){ if (!e.target) e.target = this; let n = this; while (n && !e._stop) { n._fire(e); if (!e.bubbles) break; n = n === document ? window : n === window ? null : (n.parentNode || document); } return !e.defaultPrevented; }
}

class Node_ extends EventTargetBase {
  constructor(id){ super(); this._id = id; }
  _key(){ return this._id; }
  get nodeType(){ return NODE_TYPES[N.type(this._id)]; }
  get parentNode(){ return wrap(N.parent(this._id)); }
  get parentElement(){ const p = this.parentNode; return p && p.nodeType === 1 ? p : null; }
  get childNodes(){ return ids(N.children(this._id)); }
  get firstChild(){ return this.childNodes[0] || null; }
  get lastChild(){ const c = this.childNodes; return c[c.length-1] || null; }
  get nextSibling(){ const p = this.parentNode; if (!p) return null; const c = p.childNodes; return c[c.indexOf(this)+1] || null; }
  get previousSibling(){ const p = this.parentNode; if (!p) return null; const c = p.childNodes; return c[c.indexOf(this)-1] || null; }
  get textContent(){ return N.text(this._id); }
  set textContent(v){ N.setText(this._id, String(v == null ? '' : v)); }
  get nodeValue(){ return this.nodeType === 1 ? null : N.text(this._id); }
  set nodeValue(v){ N.setText(this._id, String(v)); }
  appendChild(c){ if (c instanceof DocumentFragment) { for (const k of c.childNodes) N.append(this._id, k._id, -1); return c; } N.append(this._id, c._id, -1); return c; }
  insertBefore(c, ref){ if (c instanceof DocumentFragment) { for (const k of c.childNodes) N.append(this._id, k._id, ref ? ref._id : -1); return c; } N.append(this._id, c._id, ref ? ref._id : -1); return c; }
  removeChild(c){ N.remove(c._id); return c; }
  replaceChild(n, o){ this.insertBefore(n, o); N.remove(o._id); return o; }
  remove(){ N.remove(this._id); }
  contains(o){ for (let n = o; n; n = n.parentNode) if (n === this) return true; return false; }
  hasChildNodes(){ return this.childNodes.length > 0; }
  cloneNode(deep){ const d = document.createElement('div'); d.innerHTML = this.nodeType === 1 ? this.outerHTML : ''; const c = d.firstChild; if (c) N.remove(c._id); if (c && !deep) c.innerHTML = ''; return c; }
}
class CharacterData extends Node_ { get data(){ return N.text(this._id); } set data(v){ N.setText(this._id, String(v)); } get length(){ return this.data.length; } }

function classList(el){ const get = () => (el.getAttribute('class') || '').split(/\s+/).filter(Boolean); const set = a => el.setAttribute('class', a.join(' '));
  return { add(...c){ const a = get(); for (const x of c) if (!a.includes(x)) a.push(x); set(a); }, remove(...c){ set(get().filter(x => !c.includes(x))); },
    toggle(c, f){ const a = get(); const has = a.includes(c); const want = f === undefined ? !has : !!f; if (want && !has) a.push(c); if (!want && has) a.splice(a.indexOf(c), 1); set(a); return want; },
    contains(c){ return get().includes(c); }, replace(o, n){ const a = get(); const i = a.indexOf(o); if (i < 0) return false; a[i] = n; set(a); return true; },
    item(i){ return get()[i] || null; }, get length(){ return get().length; }, forEach(f){ get().forEach(f); }, toString(){ return get().join(' '); }, get value(){ return get().join(' '); } }; }

function styleProxy(el){ const id = el._id; const api = {
    getPropertyValue(p){ return parseStyle(N.attr(id, 'style')).get(p) || ''; },
    setProperty(p, v){ const m = parseStyle(N.attr(id, 'style')); m.set(p, v); writeStyle(id, m); },
    removeProperty(p){ const m = parseStyle(N.attr(id, 'style')); const v = m.get(p) || ''; m.delete(p); writeStyle(id, m); return v; },
    get cssText(){ return N.attr(id, 'style') || ''; }, set cssText(v){ if (v) N.setAttr(id, 'style', v); else N.removeAttr(id, 'style'); } };
  return new Proxy(api, { get(t, p){ if (p in t) return t[p]; if (typeof p !== 'string') return undefined; return parseStyle(N.attr(id, 'style')).get(kebab(p)) || ''; },
    set(t, p, v){ if (p === 'cssText') { t.cssText = v; return true; } const m = parseStyle(N.attr(id, 'style')); m.set(kebab(p), v == null ? '' : String(v)); writeStyle(id, m); return true; } }); }

function datasetProxy(el){ return new Proxy({}, { get(t, p){ if (typeof p !== 'string') return undefined; const v = el.getAttribute('data-' + kebab(p)); return v === null ? undefined : v; },
  set(t, p, v){ el.setAttribute('data-' + kebab(p), String(v)); return true; }, has(t, p){ return el.hasAttribute('data-' + kebab(p)); },
  deleteProperty(t, p){ el.removeAttribute('data-' + kebab(p)); return true; } }); }

class HTMLElement extends Node_ {
  get tagName(){ return N.tag(this._id).toUpperCase(); } get nodeName(){ return this.tagName; } get localName(){ return N.tag(this._id); }
  getAttribute(n){ const v = N.attr(this._id, String(n).toLowerCase()); return v === undefined ? null : v; }
  setAttribute(n, v){ N.setAttr(this._id, String(n).toLowerCase(), String(v)); } removeAttribute(n){ N.removeAttr(this._id, String(n).toLowerCase()); }
  hasAttribute(n){ return this.getAttribute(n) !== null; } toggleAttribute(n, f){ const h = this.hasAttribute(n); const w = f === undefined ? !h : !!f; if (w) this.setAttribute(n, ''); else this.removeAttribute(n); return w; }
  get attributes(){ return N.attrs(this._id).map(([name, value]) => ({ name, value })); }
  get id(){ return this.getAttribute('id') || ''; } set id(v){ this.setAttribute('id', v); }
  get className(){ return this.getAttribute('class') || ''; } set className(v){ this.setAttribute('class', v); }
  get classList(){ return classList(this); } get style(){ return styleProxy(this); } get dataset(){ return datasetProxy(this); }
  get innerHTML(){ return N.html(this._id, false); } set innerHTML(v){ N.setHtml(this._id, String(v == null ? '' : v)); }
  get outerHTML(){ return N.html(this._id, true); }
  set outerHTML(v){ const p = this.parentNode; if (!p) return; const d = document.createElement('div'); d.innerHTML = v; for (const c of d.childNodes) p.insertBefore(c, this); this.remove(); }
  get innerText(){ return this.textContent; } set innerText(v){ this.textContent = v; }
  get value(){ return this.getAttribute('value') || ''; } set value(v){ this.setAttribute('value', v); }
  get checked(){ return this.hasAttribute('checked'); } set checked(v){ this.toggleAttribute('checked', !!v); }
  get hidden(){ return this.hasAttribute('hidden'); } set hidden(v){ this.toggleAttribute('hidden', !!v); }
  get href(){ return this.getAttribute('href') || ''; } get src(){ return this.getAttribute('src') || ''; } set src(v){ this.setAttribute('src', v); }
  get title(){ return this.getAttribute('title') || ''; } set title(v){ this.setAttribute('title', v); }
  get children(){ return this.childNodes.filter(n => n.nodeType === 1); } get childElementCount(){ return this.children.length; }
  get firstElementChild(){ return this.children[0] || null; } get lastElementChild(){ const c = this.children; return c[c.length-1] || null; }
  get nextElementSibling(){ for (let n = this.nextSibling; n; n = n.nextSibling) if (n.nodeType === 1) return n; return null; }
  get previousElementSibling(){ for (let n = this.previousSibling; n; n = n.previousSibling) if (n.nodeType === 1) return n; return null; }
  querySelector(s){ return wrap(N.query(this._id, String(s), true)[0]); } querySelectorAll(s){ return ids(N.query(this._id, String(s), false)); }
  getElementsByClassName(c){ return this.querySelectorAll(String(c).split(/\s+/).filter(Boolean).map(x => '.' + x).join('')); }
  getElementsByTagName(t){ return this.querySelectorAll(t === '*' ? '*' : t); }
  matches(s){ return N.matches(this._id, String(s)); } closest(s){ for (let n = this; n && n.nodeType === 1; n = n.parentNode) if (n.matches(s)) return n; return null; }
  insertAdjacentHTML(pos, html){ const d = document.createElement('div'); d.innerHTML = html; const kids = d.childNodes; pos = pos.toLowerCase();
    if (pos === 'beforebegin') kids.forEach(k => this.parentNode && this.parentNode.insertBefore(k, this));
    else if (pos === 'afterbegin') { const f = this.firstChild; kids.forEach(k => this.insertBefore(k, f)); }
    else if (pos === 'beforeend') kids.forEach(k => this.appendChild(k));
    else if (pos === 'afterend') { const nx = this.nextSibling; kids.forEach(k => this.parentNode && this.parentNode.insertBefore(k, nx)); } }
  insertAdjacentElement(pos, el){ pos = pos.toLowerCase(); if (pos === 'beforeend') this.appendChild(el); else if (pos === 'afterbegin') this.insertBefore(el, this.firstChild);
    else if (pos === 'beforebegin' && this.parentNode) this.parentNode.insertBefore(el, this); else if (pos === 'afterend' && this.parentNode) this.parentNode.insertBefore(el, this.nextSibling); return el; }
  append(...n){ for (const x of n) this.appendChild(typeof x === 'string' ? document.createTextNode(x) : x); } prepend(...n){ const f = this.firstChild; for (const x of n) this.insertBefore(typeof x === 'string' ? document.createTextNode(x) : x, f); }
  before(...n){ for (const x of n) this.parentNode && this.parentNode.insertBefore(typeof x === 'string' ? document.createTextNode(x) : x, this); }
  after(...n){ const nx = this.nextSibling; for (const x of n) this.parentNode && this.parentNode.insertBefore(typeof x === 'string' ? document.createTextNode(x) : x, nx); }
  replaceWith(...n){ this.before(...n); this.remove(); }
  click(){ this.dispatchEvent(new MouseEvent('click', { bubbles: true })); } focus(){} blur(){} scrollIntoView(){} scrollTo(){} scrollBy(){}
  getBoundingClientRect(){ return { x: 0, y: 0, top: 0, left: 0, right: 0, bottom: 0, width: 0, height: 0 }; } getClientRects(){ return []; }
  get offsetHeight(){ return 0; } get offsetWidth(){ return 0; } get clientHeight(){ return 0; } get clientWidth(){ return 0; } get scrollHeight(){ return 0; } get scrollTop(){ return 0; } set scrollTop(v){}
}
class DocumentFragment extends HTMLElement {}

class Document_ extends Node_ {
  constructor(){ super(0); }
  get documentElement(){ return wrap(N.htmlId()); } get body(){ return wrap(N.bodyId()); } get head(){ return this.documentElement.firstElementChild; }
  getElementById(i){ return wrap(N.byId(String(i))); }
  querySelector(s){ return wrap(N.query(0, String(s), true)[0]); } querySelectorAll(s){ return ids(N.query(0, String(s), false)); }
  getElementsByClassName(c){ return this.documentElement.getElementsByClassName(c); } getElementsByTagName(t){ return this.documentElement.getElementsByTagName(t); }
  getElementsByName(n){ return this.querySelectorAll('[name="' + n + '"]'); }
  createElement(t){ return wrap(N.create(String(t).toLowerCase())); } createElementNS(ns, t){ return this.createElement(t); }
  createTextNode(t){ return wrap(N.createText(String(t))); } createComment(t){ return wrap(N.createText('')); }
  createDocumentFragment(){ const f = new DocumentFragment(N.create('div')); cache.set(f._id, f); return f; }
  get readyState(){ return 'complete'; } get visibilityState(){ return 'visible'; } get hidden(){ return false; } get activeElement(){ return this.body; }
  get cookie(){ return ''; } set cookie(v){} get title(){ return ''; } set title(v){}
  get defaultView(){ return window; } get location(){ return window.location; }
}
const document = new Document_(); cache.set(0, document);

function makeStorage(prefix){ const api = { getItem(k){ const v = __storage[prefix + k]; return v === undefined ? null : v; },
  setItem(k, v){ __storage[prefix + k] = String(v); }, removeItem(k){ delete __storage[prefix + k]; },
  clear(){ for (const k of Object.keys(__storage)) if (k.startsWith(prefix)) delete __storage[k]; },
  key(i){ return Object.keys(__storage).filter(k => k.startsWith(prefix)).map(k => k.slice(prefix.length))[i] || null; },
  get length(){ return Object.keys(__storage).filter(k => k.startsWith(prefix)).length; } };
  return new Proxy(api, { get(t, p){ if (p in t) return t[p]; if (typeof p === 'string') return t.getItem(p); }, set(t, p, v){ t.setItem(p, v); return true; } }); }

const timers = []; let timerSeq = 1; let clock = 0;
function addTimer(f, d, args, repeat){ if (typeof f === 'string') { const code = f; f = () => (0, eval)(code); } const id = timerSeq++; timers.push({ id, f, at: clock + Math.max(0, +d || 0), args, repeat: repeat ? Math.max(1, +d || 1) : 0 }); return id; }
globalThis.__runTimers = function(maxRuns){ let runs = 0; while (timers.length && runs < maxRuns) { timers.sort((a, b) => a.at - b.at || a.id - b.id); const t = timers.shift(); clock = Math.max(clock, t.at);
  try { t.f(...(t.args || [])); } catch (e) { console.error(String(e)); } runs++; if (t.repeat && runs < maxRuns) { t.at = clock + t.repeat; timers.push(t); } } timers.length = 0; };

class WindowTarget extends EventTargetBase { _key(){ return 'window'; } }
const win = new WindowTarget();
const g = globalThis;
for (const k of ['addEventListener', 'removeEventListener', 'dispatchEvent', '_fire', '_key']) g[k] = win[k].bind(win);
Object.assign(g, {
  window: g, self: g, top: g, parent: g, globalThis: g, document, Event, KeyboardEvent, MouseEvent, CustomEvent, HTMLElement, Element: HTMLElement, Node: Node_,
  setTimeout: (f, d, ...a) => addTimer(f, d, a, false), setInterval: (f, d, ...a) => addTimer(f, d, a, true),
  clearTimeout: id => { const i = timers.findIndex(t => t.id === id); if (i >= 0) timers.splice(i, 1); }, clearInterval: id => g.clearTimeout(id),
  requestAnimationFrame: f => addTimer(() => f(clock), 16, [], false), cancelAnimationFrame: id => g.clearTimeout(id),
  queueMicrotask: f => Promise.resolve().then(f),
  sessionStorage: makeStorage('s:'), localStorage: makeStorage('l:'),
  navigator: { userAgent: 'Mozilla/5.0 (Nintendo 3DS; Anki3DS)', platform: '3DS', language: 'en', languages: ['en'], maxTouchPoints: 1, onLine: true, clipboard: { writeText(){ return Promise.resolve(); } } },
  location: { href: 'about:blank', hash: '', search: '', pathname: '/', host: '', reload(){} }, history: { pushState(){}, replaceState(){} },
  innerWidth: 400, innerHeight: 240, outerWidth: 400, outerHeight: 240, devicePixelRatio: 1, scrollX: 0, scrollY: 0, pageYOffset: 0,
  scrollTo(){}, scrollBy(){}, alert(){}, confirm(){ return false; }, prompt(){ return null; }, open(){ return null; }, focus(){}, blur(){},
  getComputedStyle(el){ const st = el && el.style; return new Proxy({}, { get(t, p){ if (p === 'getPropertyValue') return q => (st && st.getPropertyValue(q)) || '';
    const v = st ? st[p] : ''; if (p === 'display' && !v) return 'block'; return v || ''; } }); },
  matchMedia(q){ return { matches: false, media: q, addListener(){}, removeListener(){}, addEventListener(){}, removeEventListener(){} }; },
  MutationObserver: class { observe(){} disconnect(){} takeRecords(){ return []; } }, ResizeObserver: class { observe(){} unobserve(){} disconnect(){} },
  IntersectionObserver: class { observe(){} unobserve(){} disconnect(){} },
  performance: { now(){ return clock; } }, onShownHook: [], onUpdateHook: [],
  console: { log: (...a) => N.log(a.map(String).join(' ')), info: (...a) => N.log(a.map(String).join(' ')), debug(){},
    warn: (...a) => N.log('warn: ' + a.map(String).join(' ')), error: (...a) => N.log('error: ' + a.map(String).join(' ')) },
});
globalThis.__dispatchKey = function(code, key, keyCode){ const target = document.body;
  for (const type of ['keydown', 'keyup']) { const e = new KeyboardEvent(type, { key, code, keyCode, which: keyCode, bubbles: true }); e.target = target; target.dispatchEvent(e); } };
globalThis.__beforeScripts = function(){ g.onUpdateHook = []; g.onShownHook = []; };
globalThis.__afterScripts = function(first){ if (first) for (const t of ['DOMContentLoaded', 'load']) { const e = new Event(t, { bubbles: false }); e.target = document; document._fire(e); win._fire(e); }
  for (const f of g.onUpdateHook.concat(g.onShownHook)) { try { f(); } catch (e) { console.error(String(e)); } } };
})();
)JS";

struct Ctx {
    Dom* dom = nullptr;
    std::string* log = nullptr;
    std::chrono::steady_clock::time_point deadline;
};

Dom* dom_of(JSContext* ctx) { return ((Ctx*)JS_GetContextOpaque(ctx))->dom; }

std::string str_arg(JSContext* ctx, JSValueConst v) {
    size_t len;
    const char* s = JS_ToCStringLen(ctx, &len, v);
    if (!s) return std::string();
    std::string out(s, len);
    JS_FreeCString(ctx, s);
    return out;
}

int int_arg(JSContext* ctx, JSValueConst v) {
    int32_t i = -1;
    JS_ToInt32(ctx, &i, v);
    return i;
}

JSValue id_array(JSContext* ctx, const std::vector<Node*>& nodes) {
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < nodes.size(); i++) JS_SetPropertyUint32(ctx, arr, i, JS_NewInt32(ctx, nodes[i]->id));
    return arr;
}

#define NODE_ARG(i) Node* n = argc > (i) ? dom_of(ctx)->node(int_arg(ctx, argv[i])) : nullptr; if (!n) return JS_UNDEFINED

JSValue n_type(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    NODE_ARG(0);
    int t = n->type == Node::ELEMENT ? 0 : n->type == Node::TEXT ? 1 : n->type == Node::COMMENT ? 2 : 3;
    return JS_NewInt32(ctx, t);
}
JSValue n_tag(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    NODE_ARG(0);
    return JS_NewString(ctx, n->type == Node::ELEMENT ? n->tag.c_str() : n->type == Node::TEXT ? "#text" : "#comment");
}
JSValue n_attr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    NODE_ARG(0);
    const std::string* v = n->attr(str_arg(ctx, argv[1]));
    return v ? JS_NewStringLen(ctx, v->data(), v->size()) : JS_UNDEFINED;
}
JSValue n_set_attr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    NODE_ARG(0);
    n->set_attr(str_arg(ctx, argv[1]), str_arg(ctx, argv[2]));
    dom_of(ctx)->version++;
    return JS_UNDEFINED;
}
JSValue n_remove_attr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    NODE_ARG(0);
    n->remove_attr(str_arg(ctx, argv[1]));
    dom_of(ctx)->version++;
    return JS_UNDEFINED;
}
JSValue n_attrs(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    NODE_ARG(0);
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < n->attrs.size(); i++) {
        JSValue pair = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, pair, 0, JS_NewString(ctx, n->attrs[i].first.c_str()));
        JS_SetPropertyUint32(ctx, pair, 1, JS_NewStringLen(ctx, n->attrs[i].second.data(), n->attrs[i].second.size()));
        JS_SetPropertyUint32(ctx, arr, i, pair);
    }
    return arr;
}
JSValue n_parent(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    NODE_ARG(0);
    return JS_NewInt32(ctx, n->parent ? n->parent->id : -1);
}
JSValue n_children(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    NODE_ARG(0);
    return id_array(ctx, n->children);
}
JSValue n_text(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    NODE_ARG(0);
    std::string t = dom_of(ctx)->text_content(n);
    return JS_NewStringLen(ctx, t.data(), t.size());
}
JSValue n_set_text(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    NODE_ARG(0);
    dom_of(ctx)->set_text(n, str_arg(ctx, argv[1]));
    return JS_UNDEFINED;
}
JSValue n_html(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    NODE_ARG(0);
    bool outer = argc > 1 && JS_ToBool(ctx, argv[1]);
    std::string h = outer ? dom_of(ctx)->outer_html(n) : dom_of(ctx)->inner_html(n);
    return JS_NewStringLen(ctx, h.data(), h.size());
}
JSValue n_set_html(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    NODE_ARG(0);
    dom_of(ctx)->set_inner_html(n, str_arg(ctx, argv[1]));
    return JS_UNDEFINED;
}
JSValue n_create(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return JS_NewInt32(ctx, dom_of(ctx)->create_element(argc ? str_arg(ctx, argv[0]) : "div")->id);
}
JSValue n_create_text(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return JS_NewInt32(ctx, dom_of(ctx)->create_text(argc ? str_arg(ctx, argv[0]) : "")->id);
}
JSValue n_append(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Dom* d = dom_of(ctx);
    if (argc < 2) return JS_UNDEFINED;
    Node* p = d->node(int_arg(ctx, argv[0]));
    Node* c = d->node(int_arg(ctx, argv[1]));
    Node* before = argc > 2 ? d->node(int_arg(ctx, argv[2])) : nullptr;
    // refuse cycles
    for (Node* a = p; a; a = a->parent)
        if (a == c) return JS_UNDEFINED;
    if (before && before->parent != p) before = nullptr;
    d->append(p, c, before);
    return JS_UNDEFINED;
}
JSValue n_remove(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    NODE_ARG(0);
    dom_of(ctx)->remove(n);
    return JS_UNDEFINED;
}
JSValue n_query(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    NODE_ARG(0);
    bool first = argc > 2 && JS_ToBool(ctx, argv[2]);
    return id_array(ctx, dom_of(ctx)->query(n, str_arg(ctx, argv[1]), first));
}
JSValue n_matches(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    NODE_ARG(0);
    return JS_NewBool(ctx, matches(n, str_arg(ctx, argv[1])));
}
JSValue n_by_id(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* n = argc ? dom_of(ctx)->by_id(str_arg(ctx, argv[0])) : nullptr;
    return JS_NewInt32(ctx, n ? n->id : -1);
}
JSValue n_body_id(JSContext* ctx, JSValueConst, int, JSValueConst*) { return JS_NewInt32(ctx, dom_of(ctx)->body()->id); }
JSValue n_html_id(JSContext* ctx, JSValueConst, int, JSValueConst*) { return JS_NewInt32(ctx, dom_of(ctx)->html()->id); }
JSValue n_log(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Ctx* c = (Ctx*)JS_GetContextOpaque(ctx);
    if (argc && c->log && c->log->size() < 20000) *c->log += str_arg(ctx, argv[0]) + "\n";
    return JS_UNDEFINED;
}

int interrupt(JSContext*, void* opaque) {
    Ctx* c = (Ctx*)opaque;
    return std::chrono::steady_clock::now() > c->deadline ? 1 : 0;
}

}  // namespace

struct CardScript::Impl {
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    Dom dom;
    Ctx c;
    ~Impl() {
        if (ctx) JS_FreeContext(ctx);
        if (rt) JS_FreeRuntime(rt);
    }
    void set_deadline(int ms) { c.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms); }
    bool eval(const std::string& code, const char* name, std::string& log) {
        JSValue v = JS_Eval(ctx, code.c_str(), code.size(), name, JS_EVAL_TYPE_GLOBAL);
        bool ok = !JS_IsException(v);
        if (!ok) {
            JSValue e = JS_GetException(ctx);
            std::string msg = str_arg(ctx, e);
            if (log.size() < 20000) log += std::string("error in ") + name + ": " + msg + "\n";
            JS_FreeValue(ctx, e);
        }
        JS_FreeValue(ctx, v);
        // run promise jobs
        JSContext* jc;
        for (int i = 0; i < 1000 && JS_ExecutePendingJob(rt, &jc) > 0; i++) {
        }
        return ok;
    }
};

CardScript::CardScript() = default;
CardScript::~CardScript() = default;

std::string CardScript::run(const std::string& body_html, std::map<std::string, std::string>& storage) {
    impl_ = std::make_unique<Impl>();
    log_.clear();
    storage_ = &storage;
    Impl& m = *impl_;
    m.dom.parse(body_html);
    m.rt = JS_NewRuntime();
    JS_SetMemoryLimit(m.rt, 24 * 1024 * 1024);
    JS_SetMaxStackSize(m.rt, 256 * 1024);
    m.ctx = JS_NewContext(m.rt);
    ctx_ = m.ctx;
    m.c.dom = &m.dom;
    m.c.log = &log_;
    JS_SetContextOpaque(m.ctx, &m.c);
    JS_SetInterruptHandler(m.rt, interrupt, &m.c);

    JSValue global = JS_GetGlobalObject(m.ctx);
    JSValue n = JS_NewObject(m.ctx);
    struct Fn {
        const char* name;
        JSCFunction* fn;
        int len;
    } fns[] = {{"type", n_type, 1},       {"tag", n_tag, 1},          {"attr", n_attr, 2},
               {"setAttr", n_set_attr, 3}, {"removeAttr", n_remove_attr, 2}, {"attrs", n_attrs, 1},
               {"parent", n_parent, 1},   {"children", n_children, 1}, {"text", n_text, 1},
               {"setText", n_set_text, 2}, {"html", n_html, 2},        {"setHtml", n_set_html, 2},
               {"create", n_create, 1},   {"createText", n_create_text, 1}, {"append", n_append, 3},
               {"remove", n_remove, 1},   {"query", n_query, 3},      {"matches", n_matches, 2},
               {"byId", n_by_id, 1},      {"bodyId", n_body_id, 0},   {"htmlId", n_html_id, 0},
               {"log", n_log, 1}};
    for (auto& f : fns) JS_SetPropertyStr(m.ctx, n, f.name, JS_NewCFunction(m.ctx, f.fn, f.name, f.len));
    JS_SetPropertyStr(m.ctx, global, "__n", n);
    nlohmann::json st = nlohmann::json::object();
    for (auto& [k, v] : storage) st[k] = v;
    std::string st_js = "globalThis.__storage = " + st.dump() + ";";
    JS_FreeValue(m.ctx, global);

    m.set_deadline(2000);
    m.eval(st_js, "storage", log_);
    if (!m.eval(PRELUDE, "prelude", log_)) return body_html;
    run_scripts(true);
    return m.dom.body_html();
}

void CardScript::run_scripts(bool first) {
    Impl& m = *impl_;
    m.set_deadline(500);
    m.eval("__beforeScripts();", "before", log_);
    // Run inline scripts in document order (external src= scripts are skipped).
    auto scripts = m.dom.query(m.dom.body(), "script", false);
    int idx = 0;
    for (Node* s : scripts) {
        idx++;
        if (s->attr("src")) continue;
        const std::string* type = s->attr("type");
        if (type && type->find("javascript") == std::string::npos && *type != "module" && !type->empty()) continue;
        std::string code = m.dom.text_content(s);
        m.set_deadline(1500);
        std::string name = std::string(first ? "q" : "a") + "-script" + std::to_string(idx);
        m.eval(code, name.c_str(), log_);
    }
    m.set_deadline(1500);
    m.eval(first ? "__afterScripts(true); __runTimers(500);" : "__afterScripts(false); __runTimers(500);", "after", log_);
    sync_storage_out();
}

std::string CardScript::show_answer(const std::string& body_html) {
    if (!impl_ || !impl_->ctx) return body_html;
    impl_->dom.set_inner_html(impl_->dom.body(), body_html);
    run_scripts(false);
    return impl_->dom.body_html();
}

void CardScript::sync_storage_out() {
    if (!impl_ || !impl_->ctx || !storage_) return;
    JSValue v = JS_Eval(impl_->ctx, "JSON.stringify(__storage)", 25, "storage-out", JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsException(v)) {
        std::string s = str_arg(impl_->ctx, v);
        nlohmann::json j = nlohmann::json::parse(s, nullptr, false);
        if (j.is_object()) {
            storage_->clear();
            for (auto it = j.begin(); it != j.end(); ++it)
                if (it.value().is_string()) (*storage_)[it.key()] = it.value().get<std::string>();
        }
    } else {
        JSValue e = JS_GetException(impl_->ctx);
        JS_FreeValue(impl_->ctx, e);
    }
    JS_FreeValue(impl_->ctx, v);
}

bool CardScript::key(const std::string& code) {
    if (!impl_ || !impl_->ctx) return false;
    int before = impl_->dom.version;
    int key_code = code == "ArrowLeft" ? 37 : code == "ArrowUp" ? 38 : code == "ArrowRight" ? 39 : code == "ArrowDown" ? 40
                 : code.size() == 4 && code.compare(0, 3, "Key") == 0 ? code[3] : 0;
    std::string key = code.compare(0, 3, "Key") == 0 && code.size() == 4 ? std::string(1, (char)tolower(code[3])) : code;
    nlohmann::json args = {code, key};
    std::string js = "__dispatchKey(" + args[0].dump() + ", " + args[1].dump() + ", " + std::to_string(key_code) +
                     "); __runTimers(200);";
    impl_->set_deadline(1000);
    impl_->eval(js, "key", log_);
    sync_storage_out();
    return impl_->dom.version != before;
}

std::string CardScript::eval(const std::string& expr) {
    if (!impl_ || !impl_->ctx) return "";
    impl_->set_deadline(1000);
    JSValue v = JS_Eval(impl_->ctx, expr.c_str(), expr.size(), "eval", JS_EVAL_TYPE_GLOBAL);
    std::string out;
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(impl_->ctx);
        out = "exception: " + str_arg(impl_->ctx, e);
        JS_FreeValue(impl_->ctx, e);
    } else {
        out = str_arg(impl_->ctx, v);
    }
    JS_FreeValue(impl_->ctx, v);
    return out;
}

std::string CardScript::html() const { return impl_ ? impl_->dom.body_html() : std::string(); }

}  // namespace anki::js
