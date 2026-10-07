/* View adapters exercised without a browser, using explicit text-node geometry. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const BookVoice = require('../src/octofox_library/web/voice-reader.js');
const source = fs.readFileSync('src/octofox_library/web/app.js', 'utf8');
const elements = [{classList: {add() {}, remove() {}}}, {classList: {add() {}, remove() {}}}];
const texts = ['Заголовок. ', 'Первый абзац. ', 'Продолжение с выделением.'];
const nodes = texts.map((text, index) => ({textContent: text, length: text.length, index,
  parentElement: {closest(selector) {return selector.startsWith('script') ? null : elements[index ? 1 : 0];}}}));
const article = {style: {}, scrollWidth: 3 * 360 - 44};
const scroll = {scrollTop: 0, clientWidth: 360, clientHeight: 250, setAttribute() {},
  getBoundingClientRect() {return {left: 0, width: 360, top: 0, bottom: 250};}};
const dom = {readerText: article, readerScroll: scroll, readerPage: {}, chapterSelect: {}, previousChapter: {}, nextChapter: {}};
let walk;
const doc = {
  createTreeWalker() {walk = 0; return {nextNode() {return nodes[walk++];}};},
  createRange() {
    let start, end;
    return {setStart(node, offset) {start = [node, offset];}, setEnd(node, offset) {end = [node, offset];},
      getBoundingClientRect() {
        const index = texts.slice(0, start[0].index).reduce((n, t) => n + t.length, 0) + start[1];
        const translate = Number(article.style.transform?.match(/translateX\((-?[\d.]+)px\)/)?.[1] || 0);
        return {left: Math.floor(index / 20) * 360 + 22 + translate, top: 16, bottom: 36};
      }};
  },
};
const reader = {chapter: 0, total: 2, loading: false, sequence: 0};
const context = {BookVoice, NodeFilter: {SHOW_TEXT: 4}, document: doc, $: id => dom[id],
  state: {reader, csrf: 'test'}, readerVoice: {active: true}, savePosition: async () => {},
  Date, requestAnimationFrame: cb => cb(), esc: String};
vm.createContext(context);
vm.runInContext(source.slice(source.indexOf('function createReaderPages('), source.indexOf('async function moveReaderChapter(')), context);
reader.blocks = context.readerTextBlocks();
reader.segments = reader.blocks.flatMap((b, i) => BookVoice.chunks(b.text, i));
assert.equal(reader.blocks.length, 2, 'Inline emphasis stays in its paragraph');
assert.equal(reader.blocks[1].text, texts[1] + texts[2]);
reader.pages = context.createReaderPages(scroll, article, reader.blocks, context.anchorRange);
reader.anchor = reader.pages.layout(null);
reader.pages.show(1);
assert.equal(context.visibleReaderAnchor().block, 1, 'Listening starts on the visible page');
context.followReaderAnchor({block: 1, char: 31});
assert.equal(reader.anchor.char, 31);
assert.equal(reader.pages.page, 2, 'Visual reader follows speech across page boundaries');
context.followReaderAnchor({block: 1, char: 4}, true);
assert.equal(reader.anchor.char, 4, 'Font changes restore a logical anchor, not a scroll fraction');

(async () => {
  vm.runInContext(source.slice(source.indexOf('async function loadChapter('), source.indexOf('async function savePosition(')), context);
  const original = 'Existing page'; dom.readerText.innerHTML = original;
  context.api = async () => {throw Error('offline');};
  await assert.rejects(context.loadChapter(1));
  assert.equal(reader.loading, false);
  assert.equal(dom.readerText.innerHTML, original, 'Failed chapter load retains last page');
  let complete;
  context.api = () => new Promise(resolve => {complete = resolve;});
  let current = true;
  const pending = context.loadChapter(1, 0, null, () => current);
  current = false;
  complete({chapter: 1, chapters: ['one', 'two'], html: 'Next page'});
  await pending;
  assert.equal(reader.chapter, 0, 'Cancelled speech transition cannot move the reader');
  assert.equal(dom.readerText.innerHTML, original);
  assert.equal(reader.loading, false);
  context.api = async () => ({chapter: 1, chapters: ['one', 'two'], html: 'Next page'});
  await context.loadChapter(1, 0, {block: 1, char: 4});
  assert.equal(reader.chapter, 1);
  assert.equal(reader.anchor.char, 4);
  assert.equal(reader.loading, false);
  let frame;
  context.requestAnimationFrame = fn => frame = fn;
  context.api = async () => ({chapter: 0, chapters: ['one', 'two'], html: 'Cancelled old audio page'});
  current = true;
  const oldText = dom.readerText.innerHTML;
  const pendingFrame = context.loadChapter(0, 0, null, () => current);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(reader.chapter, 1, 'Text and chapter are unchanged before the layout commit');
  current = false; frame(); await pendingFrame;
  assert.equal(reader.chapter, 1);
  assert.equal(dom.readerText.innerHTML, oldText, 'Cancelled frame cannot swap in an old audio chapter');
  assert.equal(reader.loading, false);
  const html = fs.readFileSync('src/octofox_library/web/index.html', 'utf8');
  const listen = html.match(/<button id="listenBook"[\s\S]*?<\/button>/)[0];
  assert(listen.includes('aria-label="Настройки озвучки"') && listen.includes('<svg'));
  assert(!listen.replace(/<[^>]+>/g, '').trim(), 'Listen button is an icon, not a caption');
  console.log('Voice view: text extraction, current page, inline text, page following, anchor restore, cancelled/failed chapter fetch and icon-only control OK');
})().catch(error => {console.error(error); process.exitCode = 1;});
