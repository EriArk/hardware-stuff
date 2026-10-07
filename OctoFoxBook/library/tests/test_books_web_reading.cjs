const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('src/octofox_library/web/app.js', 'utf8');
const nodes = {
  readBook: {}, bookDialog: {close() {}}, readerBookTitle: {}, readerScreen: {},
  readerScroll: {scrollTop: 450, scrollHeight: 1500, clientHeight: 500},
};
const calls = [];
const ctx = {
  state: {csrf: 'test-csrf', book: {id: '42', title: 'Книга',
    reading: {chapter: 3, offset: 0.45, shelf: 'reading'}}},
  $: id => nodes[id], document: {body: {classList: {add() {}}}},
  icon: () => '', readerPreferences() {},
  readerVoice: null, configureVoice() {}, setVoicePopover() {}, voiceState() {},
  readerScreenAwake: {setActive() {}},
  createReaderBookIndex: () => ({close() {}}),
  api: async (...args) => {calls.push(args); return {};},
};
vm.createContext(ctx);
vm.runInContext(source.slice(source.indexOf('function hasReadingPosition('),
  source.indexOf('async function loadCatalog(')), ctx);
vm.runInContext(source.slice(source.indexOf('async function openReader('),
  source.indexOf('$("closeReader").onclick')), ctx);
(async () => {
  assert(!ctx.hasReadingPosition({reading: {chapter: 0, offset: 0, shelf: ''}}));
  assert(ctx.hasReadingPosition(ctx.state.book));
  assert.equal(ctx.readingTileClass('reading', false, 0), 'book-tile continue-tile');
  for (const args of [['reading', true, 0], ['reading', false, 1], ['personal', false, 0]])
    assert.equal(ctx.readingTileClass(...args), 'book-tile');
  let opened;
  ctx.loadChapter = async (...args) => {opened = args;};
  await ctx.openReader();
  assert.deepEqual(opened, [3, 0.45, null], 'Reopening resumes the saved chapter and offset');
  assert.equal(ctx.state.book.reading.shelf, 'reading');
  ctx.state.reader.loading = false;
  calls.length = 0;
  await ctx.savePosition();
  assert.equal(calls[0][0], '/books/42/state');
  assert.equal(calls[0][1].chapter, 3);
  assert.equal(calls[0][1].offset, 0.45);
  assert.equal(calls[0][2].keepalive, true, 'Save can finish when the PWA is suspended');
  assert(!('shelf' in calls[0][1]), 'Progress saves never unset shelf/favorite');
  ctx.state.reader.anchor = {block: 8, char: 19};
  await ctx.savePosition();
  assert.deepEqual({...calls.at(-1)[1].anchor}, {block: 8, char: 19});
  assert.deepEqual({...ctx.state.book.reading.anchor}, {block: 8, char: 19});
  ctx.state.reader.pages = {offset: 0.75};
  await ctx.savePosition();
  assert.equal(calls.at(-1)[1].offset, 0.75, 'Paged progress replaces legacy vertical scroll fraction');
  assert.deepEqual({...calls.at(-1)[1].anchor}, {block: 8, char: 19});
  let releaseSave;
  ctx.api = (...args) => {
    calls.push(args);
    return calls.length === 1 ? new Promise(resolve => {releaseSave = resolve;}) : Promise.resolve();
  };
  calls.length = 0;
  ctx.state.reader.chapter = 3;
  ctx.state.reader.saveKey = null;
  const previousSave = ctx.savePosition();
  await new Promise(resolve => setImmediate(resolve));
  ctx.state.reader.chapter = 4;
  const nextSave = ctx.savePosition();
  assert.equal(calls.length, 1, 'Section saves are ordered, not racing requests');
  releaseSave(); await Promise.all([previousSave, nextSave]);
  assert.deepEqual(calls.map(call => call[1].chapter), [3, 4]);
  calls.length = 0;
  ctx.state.reader.loading = true;
  await ctx.savePosition();
  assert.equal(calls.length, 0, 'Loading placeholder cannot overwrite the saved position');
  ctx.api = async (...args) => {calls.push(args); return {};};
  ctx.readerVoice = {engine: 'server', wantsPlay: true,
    spokenPosition: {book: '42', chapter: 5, anchor: {block: 6, char: 70}}};
  await ctx.savePosition();
  assert.equal(calls.length, 0, 'Preparing a chapter must not save an unopened page');
  ctx.state.reader.loading = false;
  ctx.state.reader.chapter = 5;
  ctx.state.reader.pages.firstAnchor = () => ({block: 6, char: 0});
  await ctx.savePosition();
  assert.equal(calls.at(-1)[1].chapter, 5);
  assert.equal(calls.at(-1)[1].anchor.char, 0, 'Audio stores the visible page start, not the current phrase');
  assert.equal(ctx.state.book.reading.chapter, 5);
  const count = calls.length;
  await ctx.savePosition({book: '99', chapter: 0, anchor: {block: 0, char: 0}});
  assert.equal(calls.length, count, 'A stale voice from another book cannot overwrite progress');
  ctx.state.reader.loading = false;
  ctx.state.reader.chapter = 3; ctx.state.reader.anchor = {block: 0, char: 0};
  ctx.state.reader.pages.firstAnchor = () => ({block: 0, char: 0});
  ctx.readerVoice = {stop() { ctx.readerVoice.wantsPlay = false; }, engine: 'server', wantsPlay: false};
  ctx.renderBook = () => {}; nodes.bookDialog.showModal = () => {};
  calls.length = 0;
  await ctx.closeReader();
  assert.equal(calls.at(-1)[1].chapter, 3, 'Close remembers the page now open, not the earlier audio position');
  assert.equal(calls.at(-1)[1].anchor.char, 0);
  assert(source.includes('window.addEventListener("pagehide", flushReadingPosition)'));
  assert(source.includes('if (document.hidden) flushReadingPosition()'));
  const html = fs.readFileSync('src/octofox_library/web/index.html', 'utf8');
  for (const view of ['reading', 'favorite', 'personal'])
    assert.equal((html.match(new RegExp(`data-view="${view}"`, 'g')) || []).length, 1);
  assert(html.includes('Читаю сейчас') && html.includes('Избранное'));
  assert(source.includes('Убрать из моей библиотеки'));
  console.log('Reading: resume, keepalive autosave, loading guard and independent navigation OK');
})().catch(error => {console.error(error); process.exitCode = 1;});
