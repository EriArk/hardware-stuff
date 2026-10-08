const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const {Bookmarks} = require('../src/octofox_library/web/bookmarks.js');
const source = fs.readFileSync('src/octofox_library/web/app.js', 'utf8');
function harness() {
  class Node {
    constructor() {this.children = []; this.events = {}; this.value = ''; this.open = false;}
    append(...nodes) {this.children.push(...nodes);} replaceChildren() {this.children = [];}
    setAttribute() {} addEventListener(name, fn) {this.events[name] = fn;}
    showModal() {this.open = true;} close() {this.open = false; this.events.close?.();}
  }
  const nodes = Object.fromEntries(['open','close','dialog','form','label','add','list','status'].map(id => [id, new Node()]));
  let reader = {id: '42'}, position = {book: '42', chapter: 2, anchor: {block: 3, char: 0}, label: 'Стр. 10', excerpt: 'Отрывок'};
  const calls = [], jumps = [], errors = [];
  const item = {id: 'abc', label: '<img src=x onerror=alert(1)>', chapter: 2, anchor: {block: 3, char: 0}, excerpt: '<b>Пример</b>'};
  let api = async (url, data) => {calls.push([url, data]); return {bookmarks: [item]};};
  const env = {document: {createElement: () => new Node()}, confirm: () => true, prompt: () => 'Новое имя'};
  const panel = new Bookmarks((...args) => api(...args), {reader: () => reader, position: () => structuredClone(position),
    jump: async item => jumps.push(item), error: e => errors.push(e)}, nodes, env);
  return {panel, nodes, calls, jumps, errors, item, env, position,
    changeReader: () => reader = {id: '99'}, setApi: fn => api = fn};
}
test('bookmarks capture the open page separately, render text safely and do not alter progress on add/rename/remove', async () => {
  const h = harness(); await h.panel.open();
  assert(h.nodes.dialog.open); assert.equal(h.nodes.label.value, 'Стр. 10');
  assert.equal(h.nodes.list.children[0].children[0].children[0].textContent, h.item.label);
  h.position.chapter = 8; // Audio may keep reading while the bookmark dialog is open.
  h.nodes.label.value = 'Мой момент'; await h.panel.add(); await new Promise(setImmediate);
  assert.equal(h.calls.at(-1)[1].chapter, 2, 'The captured page, not later audio, is bookmarked');
  assert.equal(h.calls.at(-1)[1].label, 'Мой момент');
  await h.nodes.list.children[0].children[1].onclick(); await new Promise(setImmediate);
  assert.equal(h.calls.at(-1)[1].action, 'rename');
  await h.nodes.list.children[0].children[2].onclick(); await new Promise(setImmediate);
  assert.equal(h.calls.at(-1)[1].action, 'remove');
  assert(h.calls.every(([url]) => url.endsWith('/bookmarks')));
  assert.equal(h.jumps.length, 0);
  await h.nodes.list.children[0].children[0].onclick();
  assert.equal(h.nodes.dialog.open, false); assert.equal(h.jumps.length, 1);
});
test('late bookmark responses cannot populate a closed dialog or another account/book', async () => {
  const h = harness(); let resolve;
  h.setApi(() => new Promise(r => resolve = r));
  const opening = h.panel.open(); h.panel.close(); h.changeReader();
  resolve({bookmarks: [h.item]}); await opening;
  assert.equal(h.nodes.list.children.length, 0); assert.equal(h.nodes.dialog.open, false);
});
test('failed bookmark write stays visible and can be retried; repeated taps do not duplicate requests', async () => {
  const h = harness(); await h.panel.open(); let reject, writes = 0;
  h.setApi(() => {writes++; return new Promise((_, no) => reject = no);});
  const request = h.panel.mutate({action: 'add'}); h.panel.add(); h.panel.add();
  assert.equal(writes, 1); assert(h.nodes.add.disabled);
  reject(Error('Нет связи')); await request;
  assert.equal(h.nodes.status.textContent, 'Нет связи'); assert.equal(h.nodes.add.disabled, false);
  h.setApi(async () => ({bookmarks: [h.item]})); await h.panel.mutate({action: 'add'});
  assert.equal(h.nodes.list.children.length, 1);
});
test('bookmark navigation moves audio to the target and retains manual pause', async () => {
  for (const playing of [true, false]) {
    const calls = [], reader = {chapter: 0};
    const ctx = {state: {reader}, readerVoice: {engine: 'server', navigate: () => {calls.push('stop old audio'); return playing;},
      play: () => calls.push('play target')}, updateReaderPages() {}, loadChapter: async (...args) => calls.push(args)};
    vm.createContext(ctx);
    vm.runInContext(source.slice(source.indexOf('async function jumpReaderBookmark('), source.indexOf('const readerBookmarks =')), ctx);
    await ctx.jumpReaderBookmark({chapter: 7, anchor: {block: 8, char: 10}});
    assert.equal(calls[0], 'stop old audio'); assert.deepEqual(calls[1], [7, 0, {block: 8, char: 10}]);
    assert.equal(calls.includes('play target'), playing); assert.equal(reader.navigating, false);
  }
});
test('auto-resume saves the opened page start, ignores audio lookahead and keeps failed saves retryable', async () => {
  const writes = [], reader = {id: '42', chapter: 3, anchor: {block: 9, char: 50},
    pages: {firstAnchor: () => ({block: 8, char: 12}), offset: .5}};
  const ctx = {state: {reader, csrf: 'test', book: {id: '42', reading: {}}},
    readerVoice: {engine: 'server', wantsPlay: true, spokenPosition: {book: '42', chapter: 8, anchor: {block: 90, char: 0}}},
    api: async (_, data) => writes.push(data)};
  vm.createContext(ctx);
  vm.runInContext(source.slice(source.indexOf('function readerPagePosition('), source.indexOf('async function closeReader(')), ctx);
  await ctx.savePosition(ctx.readerVoice.spokenPosition);
  assert.equal(writes[0].chapter, 3); assert.equal(writes[0].anchor.block, 8); assert.equal(writes[0].anchor.char, 12);
  await ctx.savePosition(); assert.equal(writes.length, 1, 'Same-page cues do not queue duplicate saves');
  reader.loading = true; reader.chapter = 4; await ctx.savePosition(); assert.equal(writes.length, 1);
  reader.loading = false; await ctx.savePosition(); assert.equal(writes.at(-1).chapter, 4);
  reader.pages.firstAnchor = () => ({block: 10, char: 30});
  ctx.api = async () => {throw Error('offline');}; await assert.rejects(ctx.savePosition(), /offline/);
  ctx.api = async (_, data) => writes.push(data); await ctx.savePosition(); assert.equal(writes.at(-1).anchor.char, 30);
});
