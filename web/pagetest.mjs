/* pagetest.mjs — run the companion page's script without a browser.
 *
 *   node vst/web/pagetest.mjs [path/to/index.html]
 *
 * The page is 500 lines of canvas code that no compiler checks and no unit test runs; the last
 * bug of that class ("reel is not a function") only showed up when a browser rendered it. This
 * harness executes the real script against a recording canvas and a fake DOM, then checks the
 * things that break quietly:
 *
 *   1. draw() runs without throwing, in both layouts, over several frames
 *   2. every control registers a hit region (6 knobs, 3 hold switches, 12 mode cells)
 *   3. a pointerdown inside the RATE knob sends POST /set?k=rate, a pointerup sends the final value
 *   4. pressing WARP sends 1, releasing sends 0
 *   5. tapping a mode cell sends that index
 *
 * No DOM library: the stubs are the dozen things the page actually touches.
 */
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const file = process.argv[2] || new URL('./index.html', import.meta.url).pathname;
const html = readFileSync(file, 'utf8');
const m = html.match(/<script>([\s\S]*?)<\/script>/);
if (!m) { console.error('FAIL: no <script> in ' + file); process.exit(1); }
const code = m[1];

const STATE = {
  instance: 0, count: 1, active: 1, age_ms: 12, delay_ms: 140.8, bpm: 120, rate: 0.45,
  wet: 0.42, wow_pct: 1.4, mode: 2, fb: 1, sync: 0, drift: 1, jog: 0, warp: 0, twist: 0,
  echo: 0.6, mix: 0.5, intensity: 0.4, reverb: 0.2, wow: 0.15, chorus: 0,
  saturation: 0.3, dropouts: 0.1, hiss: 0.2, spread: 2,
  tapeage: 1, preamp: 1, wetonly: 0, motion: 1, twisttype: 0, warptype: 2,
  heads: [0, 0, 1], rev_head: 1
};

let calls = [];
const gradient = { addColorStop() {} };
const ctx = new Proxy({}, {
  get(_, k) {
    if (k === 'measureText') return (t) => ({ width: String(t).length * 7 });
    if (k === 'createLinearGradient' || k === 'createRadialGradient') return () => gradient;
    if (k === 'getImageData') return () => ({ data: new Uint8ClampedArray(4) });
    if (typeof k === 'string' && /^(canvas|fillStyle|strokeStyle|lineWidth|lineCap|lineJoin|font|textAlign|textBaseline|globalAlpha|shadowBlur|shadowColor)$/.test(k)) return ctx[k];
    return (...args) => { if (k === 'fill' || k === 'stroke' || k === 'fillRect') calls.push(k); };
  },
  set(_, k, v) { calls.push('set:' + k); return true; }
});

function el(extra = {}) {
  const listeners = {};
  return Object.assign({
    style: {}, classList: { add() {}, remove() {} }, className: '',
    addEventListener(t, f) { (listeners[t] = listeners[t] || []).push(f); },
    removeEventListener() {},
    appendChild() {}, remove() {}, click() {},
    getBoundingClientRect() { return { left: 0, top: 0 }; },
    setPointerCapture() {}, releasePointerCapture() {},
    _fire(t, ev) { (listeners[t] || []).forEach(f => f(ev)); }
  }, extra);
}

const canvas = el({ clientWidth: 1100, clientHeight: 660, width: 1100, height: 660, getContext: () => ctx, toDataURL: () => 'data:,' });
const bar = el();
const link = el({ firstElementChild: el() });
const host = el();
const fsBtn = el();

const document = {
  getElementById: (id) => ({ c: canvas, bar, link, host, fs: fsBtn })[id] || el(),
  createElement: () => el(),
  body: el(),
  documentElement: el({ requestFullscreen: () => Promise.resolve() }),
  addEventListener() {}
};

const fetched = [];
const sandbox = {
  console,
  document,
  window: null,
  location: { host: '192.0.2.10:8099', href: 'http://192.0.2.10:8099/' },
  navigator: {},
  performance: { now: () => Date.now() },
  devicePixelRatio: 2,
  addEventListener() {},
  setInterval: () => 1, clearInterval() {}, setTimeout: () => 1, clearTimeout() {},
  requestAnimationFrame: () => 1,
  fetch: (url, opts) => { fetched.push({ url, opts }); return Promise.resolve({ ok: true, json: () => Promise.resolve(STATE), text: () => Promise.resolve('') }); },
  Math, Date, JSON, Uint8Array, Float32Array, Object, Array, String, Number, Boolean, isNaN, parseInt, parseFloat, encodeURIComponent
};
sandbox.window = sandbox;
sandbox.globalThis = sandbox;

vm.createContext(sandbox);
vm.runInContext(code, sandbox, { filename: 'index.html' });

const P = sandbox.__mhePanel;
let fails = 0;
const check = (ok, name, detail = '') => {
  console.log((ok ? 'PASS' : 'FAIL') + ': ' + name + (detail ? ' = ' + detail : ''));
  if (!ok) fails++;
};

check(!!P, 'the page exposes its test hook', P ? 'ok' : 'missing');
if (!P) process.exit(1);

/* 1 — the drawing runs, in both layouts */
function render(w, h, frames) {
  canvas.clientWidth = w; canvas.clientHeight = h;
  P.fit();
  /* timestamps come from the same clock the page uses; synthetic ones from the past made
   * the ripple maths run backwards (a real bug, found here) */
  const t0 = performance.now();
  for (let i = 0; i < frames; i++) P.draw(t0 + i * 130);
}
try {
  render(1100, 660, 12);
  check(true, 'wide layout draws', 'no exception');
  render(430, 850, 8);
  check(true, 'portrait layout draws', 'no exception');
} catch (e) {
  check(false, 'drawing runs', String(e && e.stack ? e.stack.split('\n')[0] : e));
  process.exit(1);
}

/* 2 — every control registered a region */
render(1100, 660, 2);
const hits = P.hits();
const keys = hits.filter(h => h.t === 'knob').map(h => h.key);
for (const k of ['rate', 'intensity', 'echo', 'reverb', 'mix', 'wow']) {
  check(keys.includes(k), 'knob region: ' + k, keys.join(','));
}
const holds = hits.filter(h => h.t === 'hold').map(h => h.key);
for (const k of ['warp', 'twist', 'jog']) {
  check(holds.includes(k), 'hold region: ' + k, holds.join(','));
}
const modes = hits.filter(h => h.t === 'mode');
check(modes.length === 12, 'twelve mode cells', String(modes.length));

/* pages: three faces, reachable by swipe and by the dots */
for (let i = 0; i < 3; i++) {
  P.goPage(i);
  render(1100, 660, 3);
  check(P.page().target === i, 'page ' + i + ' is reachable', String(P.page().at));
}

/* page 1 and 2 expose their own controls */
P.goPage(1); render(1100, 660, 2);
const tapeKeys = P.hits().filter(h => h.t === 'knob' || h.t === 'seg').map(h => h.key);
for (const k of ['saturation', 'chorus', 'dropouts', 'spread', 'tapeage', 'preamp', 'wetonly', 'motion']) {
  check(tapeKeys.includes(k), 'TAPE page control: ' + k, tapeKeys.join(','));
}
P.goPage(2); render(1100, 660, 2);
const setupKeys = P.hits().filter(h => h.t === 'knob' || h.t === 'seg').map(h => h.key);
for (const k of ['sync', 'twisttype', 'warptype', 'revmode', 'drift']) {
  check(setupKeys.includes(k), 'SETUP page control: ' + k, setupKeys.join(','));
}

/* geometry: every control inside the screen, no two touching. The pages are hand-placed;
 * a 10 px mistake is invisible in a JSON diff and obvious to a thumb. */
function box(h) {
  if (h.t === 'knob' || h.t === 'hold') return { x: h.x - h.r, y: h.y - h.r, w: h.r * 2, h: h.r * 2, id: h.t + ':' + h.key };
  return { x: h.x, y: h.y, w: h.w, h: h.h, id: h.t + ':' + (h.key !== undefined ? h.key : h.i) };
}
for (let i = 0; i < 3; i++) {
  for (const [w, h] of [[1100, 660], [430, 850]]) {
    P.goPage(i); render(w, h, 2);
    const boxes = P.hits().filter(x => x.t !== 'page').map(box);
    const outside = boxes.filter(b => b.x < 0 || b.y < 0 || b.x + b.w > w || b.y + b.h > h);
    check(outside.length === 0, `page ${i} ${w}x${h}: controls inside the screen`,
          outside.map(b => b.id).slice(0, 3).join(' ') || 'ok');
    let overlaps = [];
    for (let a = 0; a < boxes.length; a++)
      for (let b = a + 1; b < boxes.length; b++) {
        const A = boxes[a], B = boxes[b];
        if (A.x < B.x + B.w && B.x < A.x + A.w && A.y < B.y + B.h && B.y < A.y + A.h) overlaps.push(A.id + ' x ' + B.id);
      }
    check(overlaps.length === 0, `page ${i} ${w}x${h}: no overlapping controls`, overlaps.slice(0, 3).join(' | ') || 'ok');
  }
}

/* a segment tap sends its index */
P.goPage(2); render(1100, 660, 2);
const syncSeg = P.hits().find(h => h.t === 'seg' && h.key === 'sync' && h.i === 3);
fetched.length = 0;
canvas._fire('pointerdown', { clientX: syncSeg.x + 3, clientY: syncSeg.y + 3, pointerId: 9, preventDefault() {} });
check(fetched.some(f => f.url.includes('k=sync') && f.url.includes('v=3')), 'a segment tap sends its index',
      fetched.map(f => f.url).join(' '));

/* a swipe on the bare panel moves to the next page */
P.goPage(0); render(1100, 660, 2);
/* the finger moves left (700 -> 40): the next page comes in, as on a phone */
canvas._fire('pointerdown', { clientX: 700, clientY: 300, pointerId: 10, preventDefault() {} });
canvas._fire('pointermove', { clientX: 40, clientY: 300, pointerId: 10 });
canvas._fire('pointerup', { clientX: 40, clientY: 300, pointerId: 10 });
check(P.page().target === 1, 'a leftward swipe turns to the next page', JSON.stringify(P.page()));
/* and back again */
render(1100, 660, 20);
canvas._fire('pointerdown', { clientX: 60, clientY: 300, pointerId: 11, preventDefault() {} });
canvas._fire('pointermove', { clientX: 720, clientY: 300, pointerId: 11 });
canvas._fire('pointerup', { clientX: 720, clientY: 300, pointerId: 11 });
check(P.page().target === 0, 'a rightward swipe goes back', JSON.stringify(P.page()));

/* 3 — a knob drag writes */
P.goPage(0); render(1100, 660, 2);
const rateKnob = hits.find(h => h.t === 'knob' && h.key === 'rate');
fetched.length = 0;
canvas._fire('pointerdown', { clientX: rateKnob.x, clientY: rateKnob.y, pointerId: 1, preventDefault() {} });
canvas._fire('pointermove', { clientX: rateKnob.x, clientY: rateKnob.y - 90, pointerId: 1, preventDefault() {} });
canvas._fire('pointerup', { clientX: rateKnob.x, clientY: rateKnob.y - 90, pointerId: 1 });
const rateUrls = fetched.map(f => f.url).filter(u => u.includes('k=rate'));
check(rateUrls.length > 0, 'turning the RATE knob sends /set?k=rate', rateUrls[rateUrls.length - 1] || 'none');
const rateVal = rateUrls.length ? parseFloat(rateUrls[rateUrls.length - 1].split('v=')[1]) : -1;
check(rateVal > rateKnob.v, 'the value went up', String(rateVal));
const rateCall = fetched.find(f => f.url === rateUrls[0]);
check(rateCall.opts && rateCall.opts.method === 'POST', 'writes are POST', 'ok');
check(rateCall.opts.headers && rateCall.opts.headers['X-MHE'] === '1',
      'writes carry the X-MHE header (no cross-site form can set one)', JSON.stringify(rateCall.opts.headers || {}));

/* 4 — a hold switch presses and releases */
const warpHit = hits.find(h => h.t === 'hold' && h.key === 'warp');
fetched.length = 0;
canvas._fire('pointerdown', { clientX: warpHit.x, clientY: warpHit.y, pointerId: 2, preventDefault() {} });
canvas._fire('pointerup', { clientX: warpHit.x, clientY: warpHit.y, pointerId: 2 });
const warpVals = fetched.map(f => f.url).filter(u => u.includes('k=warp')).map(u => u.split('v=')[1]);
check(warpVals[0] === '1' && warpVals[warpVals.length - 1] === '0', 'WARP presses to 1 and releases to 0', warpVals.join(' -> '));

/* 5 — the mode grid */
const cell = hits.find(h => h.t === 'mode' && h.i === 10);
fetched.length = 0;
canvas._fire('pointerdown', { clientX: cell.x + 2, clientY: cell.y + 2, pointerId: 3, preventDefault() {} });
check(fetched.some(f => f.url.includes('k=mode') && f.url.includes('v=10')), 'a mode cell sends its index',
      fetched.map(f => f.url).join(' '));

console.log(fails ? 'FAILED' : 'PASSED');
process.exit(fails ? 1 : 0);
