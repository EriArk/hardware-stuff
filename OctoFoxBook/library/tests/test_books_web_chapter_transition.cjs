const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const {PageVoice} = require('../src/octofox_library/web/page-voice.js');
const source = fs.readFileSync('src/octofox_library/web/app.js', 'utf8');

// Explicit text/column geometry with real ancestor traversal. Not browser QA.
function harness() {
  class Element {
    constructor(tag) { this.tag = tag; this.style = {}; this.attrs = {}; this.children = []; }
    append(child) { child.parentElement = this; this.children.push(child); }
    contains(node) { for (; node; node = node.parentElement) if (node === this) return true; return false; }
    setAttribute(key, value) { this.attrs[key] = value; }
    closest(selector) {
      for (let el = this; el; el = el.parentElement) {
        if (selector.startsWith('script') ? ['script','style','img'].includes(el.tag) || el.attrs['aria-hidden'] === 'true'
          : ['p','h1','h2','h3','h4','blockquote','li'].includes(el.tag)) return el;
      }
      return null;
    }
    remove() { this.parentElement.children = this.parentElement.children.filter(child => child !== this); }
    set innerHTML(text) {
      this.children = [];
      const p = new Element('p'); p.append({textContent: text, length: text.length}); this.append(p);
    }
    get clientWidth() { return parseInt(this.style.width) || 390; }
    get clientHeight() { return parseInt(this.style.height) || 640; }
    get scrollWidth() { return Math.ceil((this.children[0]?.children[0]?.length || 1) / 20) * 390 - 44; }
    getBoundingClientRect() { return {left: this.attrs['aria-hidden'] ? -10000 : 0}; }
  }
  const screen = new Element('section'), viewport = new Element('div'), article = new Element('article');
  screen.append(viewport); viewport.append(article); article.innerHTML = 'я'.repeat(40);
  const dom = {readerScreen: screen, readerScroll: viewport, readerText: article, readerPage: {},
    previousChapter: {}, nextChapter: {}, chapterSelect: {}};
  const document = {
    createElement: tag => new Element(tag),
    createTreeWalker(root) {
      const nodes = [];
      const visit = el => { for (const child of el.children || []) child.textContent === undefined ? visit(child) : nodes.push(child); };
      visit(root); let i = 0; return {nextNode: () => nodes[i++]};
    },
    createRange() {
      let node, char;
      return {setStart(n, c) {node = n; char = c;}, setEnd() {}, getBoundingClientRect() {
        const article = node.parentElement.parentElement, viewport = article.parentElement;
        const translate = Number(article.style.transform?.match(/translateX\((-?[\d.]+)px\)/)?.[1] || 0);
        return {left: viewport.getBoundingClientRect().left + 22 + Math.floor(char / 20) * 390 + translate};
      }};
    },
  };
  const reader = {id: '42', chapter: 0, total: 3, sequence: 0, loading: false};
  const ctx = {state: {reader}, readerVoice: null, document, NodeFilter: {SHOW_TEXT: 4}, $: id => dom[id],
    readerScreenAwake: {setActive() {}},
    getComputedStyle: () => ({fontSize: '20px'}), requestAnimationFrame: fn => fn(),
    api: async () => ({chapter: 1, chapters: ['A','B','C'], html: 'б'.repeat(120)}),
    savePosition: async () => {}, BookVoice: {chunks: () => []}, esc: String};
  vm.createContext(ctx);
  vm.runInContext(source.slice(source.indexOf('function createReaderPages('), source.indexOf('async function moveReaderChapter(')), ctx);
  vm.runInContext(source.slice(source.indexOf('async function loadChapter('), source.indexOf('async function savePosition(')), ctx);
  reader.blocks = ctx.readerTextBlocks(article);
  reader.pages = ctx.createReaderPages(viewport, article, reader.blocks, ctx.anchorRange);
  reader.anchor = reader.pages.layout(null);
  return {ctx, reader, dom, Element};
}

test('hidden chapter measurement extracts the same text as visible reading; hidden book content stays excluded', () => {
  const h = harness(), visible = h.ctx.readerTextBlocks();
  h.dom.readerScroll.setAttribute('aria-hidden', 'true');
  h.dom.readerScroll.setAttribute('inert', '');
  const measured = h.ctx.readerTextBlocks();
  assert.deepEqual(measured.map(b => b.text), visible.map(b => b.text));
  h.dom.readerText.children[0].setAttribute('aria-hidden', 'true');
  assert.equal(h.ctx.readerTextBlocks().length, 0);
});

test('actual next-chapter adapter returns nonempty contiguous pages instead of silently skipping the chapter', async () => {
  const h = harness(); let page = h.ctx.speechPage();
  page = await h.ctx.nextSpeechPage(page);
  assert.equal(page.chapter, 0); assert.equal(page.page, 1);
  for (let i = 0; i < 6; i++) {
    page = await h.ctx.nextSpeechPage(page);
    assert.equal(page.chapter, 1); assert.equal(page.page, i);
    assert.equal(page.empty, false);
    assert.equal(page.anchor.char, i * 20); assert.equal(page.end.char, (i + 1) * 20);
  }
  assert.equal(h.dom.readerScreen.children.length, 1, 'Measurement nodes are removed');
  assert.equal(h.reader.chapter, 0, 'Lookahead never moves the visible reader');
});

test('page audio uses the measured next chapter and never races through it as empty pages', async () => {
  const h = harness(), statuses = new Map(), saves = [];
  let id = 0, now = 0;
  class Audio {
    constructor() {this.currentTime = 0; this.duration = 1; this.paused = true;}
    set src(value) {this.currentSrc = value; this.currentTime = 0; this.ended = false;}
    load() {} play() {this.paused = false; return Promise.resolve();} pause() {this.paused = true;} removeAttribute() {}
  }
  const env = {Audio, Date: {now: () => now}, crypto: {randomUUID: () => String(++id)},
    setTimeout: () => 1, clearTimeout() {}};
  const voice = new PageVoice(async (url, data) => {
    if (url.endsWith('/speech')) statuses.set(data.id, {complete: true, segments: [{at: 0, duration: 1, cues: [{start: 0, anchor: data.anchor}]}]});
    if (url.endsWith('/status')) return statuses.get(url.split('/')[3]);
    return {};
  }, {page: h.ctx.speechPage, next: h.ctx.nextSpeechPage, state() {}, save: p => saves.push(p),
    follow: async (p, current) => {
      if (h.reader.chapter !== p.chapter) await h.ctx.loadChapter(p.chapter, 0, p.anchor, current);
      if (!current()) return false;
      h.reader.pages.follow(p.anchor); return true;
    }}, {available: true}, env);
  const tick = () => new Promise(setImmediate);
  voice.play(); await tick();
  for (let i = 0; i < 2; i++) {
    voice.audio.onloadedmetadata(); voice.audio.onplaying(); await tick();
    now += 2000; voice.audio.currentTime = 1; voice.audio.ended = true; voice.audio.onended(); await tick();
  }
  assert.equal(voice.current.page.chapter, 1);
  assert.equal(voice.current.page.page, 0);
  assert.equal(voice.current.page.empty, false);
  voice.audio.onloadedmetadata(); voice.audio.onplaying(); await tick();
  assert.equal(voice.status, 'playing'); assert.equal(voice.wantsPlay, true);
  assert.equal(h.reader.chapter, 1); assert.equal(h.reader.pages.page, 0);
  assert.equal(h.reader.loading, false);
  assert(saves.every(p => p.chapter <= 1), 'No runaway bookmark writes to later chapters');
  assert.equal(voice.entries.size, 4);
  voice.stop();
});

test('chapter layout exception rejects and releases navigation instead of leaving a pending frame forever', async () => {
  const h = harness(); let frame;
  h.ctx.requestAnimationFrame = fn => {frame = fn;};
  h.ctx.BookVoice.chunks = () => {throw Error('layout failed');};
  const pending = h.ctx.loadChapter(1);
  const rejection = assert.rejects(pending, /layout failed/);
  await new Promise(setImmediate);
  assert.doesNotThrow(() => frame());
  await rejection;
  assert.equal(h.reader.loading, false);
  assert.equal(h.dom.chapterSelect.disabled, false);
});

test('reader closes immediately even when a bookmark save is stalled', async () => {
  const h = harness(); let release;
  h.ctx.savePosition = () => new Promise(resolve => {release = resolve;});
  h.ctx.setVoicePopover = h.ctx.renderBook = h.ctx.toast = () => {};
  h.dom.bookDialog = {showModal() {this.open = true;}};
  h.ctx.document.body = {classList: {add() {}}};
  vm.runInContext(source.slice(source.indexOf('async function closeReader('), source.indexOf('$("closeReader").onclick')), h.ctx);
  const closing = h.ctx.closeReader();
  assert.equal(h.dom.readerScreen.hidden, true);
  assert.equal(h.ctx.state.reader, null);
  assert.equal(h.dom.bookDialog.open, true);
  release(); await closing;
});

test('reopening a book keeps old/new bookmark writes ordered; changed login cancels pending old-account writes', async () => {
  const h = harness(), calls = []; let release;
  h.ctx.state.csrf = 'session-1';
  h.ctx.api = async (url, position) => {
    calls.push(position.chapter);
    if (calls.length === 1) await new Promise(resolve => {release = resolve;});
  };
  vm.runInContext(source.slice(source.indexOf('async function savePosition('), source.indexOf('async function closeReader(')), h.ctx);
  const oldSave = h.ctx.savePosition(); await new Promise(setImmediate);
  h.ctx.state.reader = {...h.reader, chapter: 1};
  const newSave = h.ctx.savePosition(); await new Promise(setImmediate);
  assert.deepEqual(calls, [0]);
  release(); await Promise.all([oldSave, newSave]);
  assert.deepEqual(calls, [0, 1]);
  assert.equal(h.ctx.savePosition.queues.size, 0);
  calls.length = 0;
  h.ctx.state.reader.saveKey = null;
  const started = h.ctx.savePosition(); await new Promise(setImmediate);
  h.ctx.state.reader.chapter = 2;
  const queued = h.ctx.savePosition();
  h.ctx.state.csrf = 'session-2';
  release(); await Promise.all([started, queued]);
  assert.deepEqual(calls, [1], 'No queued bookmark is sent under another session');
});
