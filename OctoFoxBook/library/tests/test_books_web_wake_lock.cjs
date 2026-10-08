const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('src/octofox_library/web/app.js', 'utf8');
const tick = () => new Promise(setImmediate);
function harness(supported = true) {
  const events = () => ({handlers: {}, addEventListener(name, fn) {
    (this.handlers[name] ||= []).push(fn);
  }, emit(name) { for (const fn of this.handlers[name] || []) fn(); }});
  const doc = {...events(), hidden: false}, win = events(), requests = [];
  let now = 0;
  const nav = supported ? {wakeLock: {request(type) {
    assert.equal(type, 'screen');
    return new Promise((resolve, reject) => requests.push({resolve, reject}));
  }}} : {};
  const ctx = {}; vm.createContext(ctx);
  vm.runInContext(source.slice(source.indexOf('function createReaderScreenAwake('),
    source.indexOf('async function openReader(')), ctx);
  const control = ctx.createReaderScreenAwake(nav, doc, win, () => now);
  function sentinel() {
    return {...events(), released: false, releases: 0, release() {
      this.releases++; this.released = true; this.emit('release'); return Promise.resolve();
    }};
  }
  return {doc, win, requests, control, sentinel, time: value => {now = value;}};
}
test('screen is held only inside reader; navigation and audio do not create extra locks', async () => {
  const h = harness();
  h.doc.emit('pointerup'); h.doc.emit('keydown'); await tick();
  assert.equal(h.requests.length, 0);
  h.control.setActive(true); await tick();
  const lock = h.sentinel(); h.requests[0].resolve(lock); await tick();
  h.control.setActive(true); h.doc.emit('pointerup'); await tick();
  assert.equal(h.requests.length, 1);
  h.control.setActive(false); assert.equal(lock.releases, 1);
});
test('pending permission result after closing is released without keeping screen awake', async () => {
  const h = harness(); h.control.setActive(true); await tick();
  h.control.setActive(false);
  const lock = h.sentinel(); h.requests[0].resolve(lock); await tick();
  assert.equal(lock.releases, 1); assert.equal(h.requests.length, 1);
});
test('background releases, returning to reader reacquires on Android and iOS alike', async () => {
  const h = harness(); h.control.setActive(true); await tick();
  const lock = h.sentinel(); h.requests[0].resolve(lock); await tick();
  h.doc.hidden = true; h.doc.emit('visibilitychange');
  assert.equal(lock.releases, 1);
  h.doc.emit('pointerup'); await tick(); assert.equal(h.requests.length, 1);
  h.doc.hidden = false; h.doc.emit('visibilitychange'); await tick();
  assert.equal(h.requests.length, 2);
});
test('rapid background/foreground serializes stale and new acquisitions', async () => {
  const h = harness(); h.control.setActive(true); await tick();
  h.doc.hidden = true; h.doc.emit('visibilitychange');
  h.doc.hidden = false; h.doc.emit('visibilitychange');
  const old = h.sentinel(); h.requests[0].resolve(old); await tick();
  assert.equal(old.releases, 1); assert.equal(h.requests.length, 2);
  const current = h.sentinel(); h.requests[1].resolve(current); await tick();
  old.emit('release'); h.doc.emit('pointerup'); await tick();
  assert.equal(h.requests.length, 2);
  h.control.setActive(false); assert.equal(current.releases, 1);
});
test('pagehide suspends and pageshow restores reader lock (BFCache)', async () => {
  const h = harness(); h.control.setActive(true); await tick();
  const lock = h.sentinel(); h.requests[0].resolve(lock); await tick();
  h.win.emit('pagehide'); assert.equal(lock.releases, 1);
  h.doc.emit('pointerup'); await tick(); assert.equal(h.requests.length, 1);
  h.win.emit('pageshow'); await tick(); assert.equal(h.requests.length, 2);
});
test('OS release is respected without a spin loop; next user action can retry', async () => {
  const h = harness(); h.control.setActive(true); await tick();
  const lock = h.sentinel(); h.requests[0].resolve(lock); await tick();
  await lock.release(); await tick(); assert.equal(h.requests.length, 1);
  h.doc.emit('pointerup'); await tick(); assert.equal(h.requests.length, 2);
});
test('unsupported or denied API does not break reading and retries are throttled', async () => {
  const unsupported = harness(false); unsupported.control.setActive(true); await tick();
  assert.equal(unsupported.requests.length, 0);
  const h = harness(); h.control.setActive(true); await tick();
  h.requests[0].reject(Error('NotAllowedError')); await tick();
  h.doc.emit('pointerup'); await tick(); assert.equal(h.requests.length, 1);
  h.time(5000); h.doc.emit('keydown'); await tick(); assert.equal(h.requests.length, 2);
});
test('failed release is harmless; lifecycle hooks exist on login, open and close', async () => {
  const h = harness(); h.control.setActive(true); await tick();
  h.requests[0].resolve({...h.sentinel(), release() {return Promise.reject(Error('released'));}});
  await tick(); h.control.setActive(false); await tick();
  assert.match(source, /function showLogin\(\)\s*\{\s*readerScreenAwake.setActive\(false\)/);
  assert.match(source, /async function closeReader\(\)\s*\{\s*readerScreenAwake.setActive\(false\)/);
  const open = source.slice(source.indexOf('async function openReader('), source.indexOf('async function loadChapter('));
  assert(open.includes('readerScreenAwake.setActive(true)'));
  assert(open.includes('readerScreenAwake.setActive(false)'));
});
