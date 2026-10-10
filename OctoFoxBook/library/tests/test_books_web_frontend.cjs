/* Fast regression checks for the real frontend helpers; no browser dependencies. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const root = path.join(__dirname, '../src/octofox_library/web');
const source = fs.readFileSync(path.join(root, 'app.js'), 'utf8');
const html = fs.readFileSync(path.join(root, 'index.html'), 'utf8');
const css = fs.readFileSync(path.join(root, 'styles.css'), 'utf8');
const ctx = {icon: name => `<svg><use href="#i-${name}"/></svg>`};
vm.createContext(ctx);
// Use the real escaping helper now that the fallback includes metadata text.
vm.runInContext(source.slice(source.indexOf('const esc ='), source.indexOf('const icon =')), ctx);
vm.runInContext(source.slice(source.indexOf('function cover('), source.indexOf('async function loadCatalog(')), ctx);
for (const lazy of [true, false]) {
  const result = ctx.cover({title: 'Книга', cover: '/reader-api/books/42/cover'}, lazy);
  const img = result.match(/<img\b[^>]*>/)[0];
  assert(!/\bhidden\b/.test(img), 'A hidden lazy image never becomes eligible for loading');
  assert(img.includes('width="240"') && img.includes('height="360"'));
  assert(img.includes(`loading="${lazy ? 'lazy' : 'eager'}"`));
}
const classes = new Set();
const img = {complete: false, naturalWidth: 0, previousElementSibling: {hidden: false,
  dataset: {fallbackCover: ctx.fallbackCover({id: '42'})}, style: {}},
  classList: {add: c => classes.add(c)}, remove: () => {img.removed = true;}};
ctx.bindCovers({querySelectorAll: () => [img]});
assert(!classes.has('loaded'));
img.onload();
assert(classes.has('loaded'));
assert(img.previousElementSibling.hidden);
classes.clear();
img.complete = true; img.naturalWidth = 240;
ctx.bindCovers({querySelectorAll: () => [img]});
assert(classes.has('loaded'), 'Already cached covers must be revealed');
img.onerror();
assert(img.removed);
assert(!img.previousElementSibling.hidden, 'Failed cover must reveal decorative fallback');
assert.equal(img.previousElementSibling.style.backgroundImage, `url('${ctx.fallbackCover({id: '42'})}')`);
assert.equal(ctx.fallbackCover({id: '42', title: 'Old'}), ctx.fallbackCover({id: '42', title: 'New'}));
assert.equal(ctx.fallbackCover({title: '\u00e9'}), ctx.fallbackCover({title: 'e\u0301'}));
const variants = new Set(Array.from({length: 1000}, (_, id) => ctx.fallbackCover({id: String(id + 1)})));
assert.equal(variants.size, 16, 'The complete placeholder set is used');
for (const art of variants) assert.match(art, /^\/cover-art\/v1\/(0[1-9]|1[0-6])\.webp$/);
assert(ctx.cover({id: '42', title: 'Book'}).includes('style="background-image:'));
assert(!ctx.cover({id: '42', title: 'Book', cover: '/real.jpg'}).includes('style="background-image:'),
  'Do not download fallback art for books with a working real cover');
const missing = ctx.cover({title: '<img src=x onerror=alert(1)>'});
assert(missing.includes('href="#i-book"'), 'Placeholder retains its book icon');
assert(missing.includes('class="cover-fallback-title">&lt;img src=x onerror=alert(1)&gt;</span>'));
assert(!missing.includes('<img '), 'Metadata cannot inject fallback markup');
assert(ctx.cover({title: 'Книга'}).includes('class="cover-fallback-title">Книга</span>'));
assert(css.includes('.cover-fallback-content {') && css.includes('max-height: 62%;'),
  'Fallback label must stay within the fixed cover, above its bottom caption');
assert(/\.cover-wrap img\.loaded\s*\{\s*opacity:\s*1;/.test(css));
// Text (including missing-cover fallback) must not size compact book tiles.
const tileSelector = '.book-grid > .book-tile:not(.continue-tile)';
function tileRule(suffix = '') {
  const start = css.indexOf(tileSelector + suffix + ' {');
  assert(start >= 0, `Missing uniform tile rule: ${suffix}`);
  return css.slice(start, css.indexOf('}', start));
}
assert(tileRule().includes('aspect-ratio: 2 / 3;'));
assert(tileRule().includes('position: relative;'));
assert(!tileRule().includes('grid-template-rows'), 'No empty tracks below the cover');
assert(tileRule(' > .cover-wrap').includes('aspect-ratio: 2 / 3;'));
assert(tileRule(' > .cover-wrap').includes('position: absolute;'));
assert(tileRule(' .cover-fallback').includes('position: absolute;'));
assert(css.includes('background-size: cover;'));
assert(tileRule(' .tile-caption h3').includes('-webkit-line-clamp: 2;'));
assert(tileRule(' .tile-caption h3').includes('max-height: 2.7em;'));
assert(tileRule(' .tile-caption p').includes('-webkit-line-clamp: 1;'));
assert(tileRule(' .tile-caption p').includes('line-height: 1.4;'));
assert(tileRule(' .tile-caption p').includes('max-height: 1.4em;'));
assert(css.includes('inset: auto 0 0;'));
assert(source.includes('<div class="tile-caption">${caption}</div>'));
assert(css.includes('--tile-title-size: 16px;'), 'Desktop title track scales with its font');
assert(css.includes('gap: 14px 12px;'));
assert(Number(html.match(/\/styles\.css\?v=(\d+)/)?.[1]) >= 42, 'Installed web apps must request the updated CSS');
const fallbackLabel = css.match(/\.cover-fallback-content\s*\{([^}]+)\}/)[1];
assert(!/background|border-radius|box-shadow/.test(fallbackLabel), 'No backing panel on decorative covers');
assert(/\.cover-fallback-content svg\s*\{[^}]*width: 44px;[^}]*height: 44px;/.test(css));
assert(css.includes("font: 600 1.75rem/1.08 'Cover Cormorant', Georgia, serif;"));
assert(css.includes("url('/fonts/cormorant-garamond-600-cyrillic-v1.woff2')"));
assert(tileRule().includes('border-radius: 12px;'));
const outline = tileRule('::after');
assert(outline.includes('border: 1px solid rgba(255, 248, 236, .42);'));
assert(outline.includes('z-index: 3;'), 'The frame must remain visible above the caption');
assert(outline.includes('pointer-events: none;'), 'The outline must not intercept card selection');
assert(css.includes('background: rgba(16, 12, 16, .82);'));
const filterPanel = html.slice(html.indexOf('<div id="filters"'), html.indexOf('<div id="activeFilters"'));
assert(filterPanel.startsWith('<div id="filters" class="filters" hidden>'));
for (const id of ['searchScope', 'sortFilter', 'genreChoice', 'authorChoice', 'seriesChoice', 'tagChoice'])
  assert(filterPanel.includes(`id="${id}"`), `Advanced option outside collapsed filters: ${id}`);
assert(css.includes('min-height: 44px'), 'Compact spacing must retain touch-sized controls');
const ids = [...html.matchAll(/\bid="([^"]+)"/g)].map(m => m[1]);
assert.equal(ids.length, new Set(ids).size, 'Duplicate HTML id');
for (const [, id] of source.matchAll(/\$\("([^"$]+)"\)/g)) {
  assert(ids.includes(id) || source.includes(`id="${id}"`), `Missing element: ${id}`);
}
assert(!html.includes('<datalist'), 'Do not ship thousands of names as a mobile picker');
ctx.state = {view: 'personal', filters: {genre: {value: 'sf_fantasy'}}};
ctx.$ = id => ({value: {sortFilter: 'recent', searchScope: 'title', searchInput: 'Солнце'}[id]});
ctx.URLSearchParams = URLSearchParams;
vm.runInContext(source.slice(source.indexOf('function parameters('), source.indexOf('function cover(')), ctx);
let params = ctx.parameters(1);
assert.equal(params.get('personal'), '1');
assert.equal(params.get('shelf'), null);
assert.equal(params.get('genre'), 'sf_fantasy');
ctx.state.view = 'favorite';
params = ctx.parameters(1);
assert.equal(params.get('personal'), null);
assert.equal(params.get('shelf'), 'favorite');
ctx.state.view = 'recent';
params = ctx.parameters(1);
assert.equal(params.get('new'), '1');
assert.equal(params.get('shelf'), null);
assert.equal(params.get('personal'), null);
assert(html.includes('</svg>Новинки'));
ctx.document = {hidden: false};
ctx.state.csrf = 'test';
ctx.state.catalogRevision = 10;
let checks = 0;
ctx.api = async () => { checks++; return {status: 'ready', revision: 11}; };
const updateButton = {hidden: true};
ctx.$ = () => updateButton;
ctx.daemonAdmin = {catalog(status, changed) {updateButton.hidden = !changed && status !== 'error';}};
vm.runInContext(source.slice(source.indexOf('async function checkCatalogUpdates('), source.indexOf('$("catalogUpdates").onclick')), ctx);
(async () => {
  await ctx.checkCatalogUpdates();
  assert.equal(updateButton.hidden, false);
  ctx.state.catalogRevision = 11;
  await ctx.checkCatalogUpdates();
  assert.equal(updateButton.hidden, true);
  ctx.document.hidden = true;
  await ctx.checkCatalogUpdates();
  assert.equal(checks, 2, 'Hidden app must not poll');
})().catch(error => { console.error(error); process.exitCode = 1; });
assert(html.includes('data-view="personal"'));
assert(source.includes('/download" download target="_blank" rel="noopener"'));
console.log('Frontend: lazy/eager covers, cached/error states, markup IDs and searchable directories OK');
