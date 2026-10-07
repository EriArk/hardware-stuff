const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const css = fs.readFileSync('src/octofox_library/web/styles.css','utf8');
const rule = selector => css.slice(css.indexOf(selector + ' {')).split('}')[0];
test('reader fills fixed viewport edges rather than imposing a second viewport height', () => {
  const r=rule('.reader-screen');
  assert.match(r,/position: fixed/); assert.match(r,/inset: 0/);
  assert.match(r,/height: auto/); assert(!r.includes('100dvh'));
  assert.match(r,/padding: 0/); assert.match(r,/flex-direction: column/);
  assert.match(rule('.reader-scroll'),/flex: 1/);
});
test('footer is one 44px row plus only system safe area, with matching reader backdrop', () => {
  const r=rule('.reader-bottom');
  assert.match(r,/gap: 0 6px/); assert.match(r,/padding: 0 max/);
  assert.match(r,/env\(safe-area-inset-bottom\)/);
  assert.match(r,/background: var\(--paper\)/);
  assert.match(rule('.reader-bottom > .icon-button'),/height: 44px/);
  assert(css.includes('body:has(.reader-screen.night:not([hidden])) { background: #151318; }'));
  assert(css.includes('body:has(.reader-screen:not([hidden])) { background: #f5eddd; }'));
});
