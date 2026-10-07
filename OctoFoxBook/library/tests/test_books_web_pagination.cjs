const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('src/octofox_library/web/app.js', 'utf8');

// Explicit column geometry, not a browser layout substitute. Test the actual adapters.
function harness({legacyWebkit = false} = {}) {
  let chars = 20, now = 1000, saveCount = 0, layoutCalls = 0;
  const events = {}, frames = [], requests = [];
  const blocks = [{text: ' '.repeat(3)}, {text: 'я'.repeat(81)}, {text: 'конец'}];
  const starts = [0, 0, 81], total = 86;
  const article = {style: {}, get scrollWidth() {
    // WebKit before Safari 26.2 did not establish multicol for column-count: 1
    // without an explicit column-width. It reported one width despite vertical overflow.
    if (legacyWebkit && this.style.columnWidth !== this.style.width) return viewport.clientWidth - 44;
    return Math.ceil(total / chars) * (viewport.clientWidth >= 900 ? viewport.clientWidth / 2 : viewport.clientWidth) - 44;
  }};
  const viewport = {clientWidth: 390, clientHeight: 640, setAttribute() {},
    getBoundingClientRect() { return {left: 20, width: this.clientWidth}; }};
  const nodes = {readerText: article, readerScroll: viewport, readerPage: {}, chapterSelect: {}, previousChapter: {}, nextChapter: {}};
  const ctx = {state: {reader: {chapter: 0, total: 3, loading: false, blocks}}, readerVoice: null,
    $: id => nodes[id], requestAnimationFrame: fn => frames.push(fn),
    Date: {now: () => now}, Math, positionTimer: null, setTimeout: () => 1, clearTimeout() {},
    savePosition: async () => {saveCount++;}, task: fn => fn,
    window: {getSelection: () => ({isCollapsed: true})},
    document: {addEventListener: (name, fn) => {events[name] = fn;}},
    anchorRange: anchor => ({getBoundingClientRect() {
      const translate = Number(article.style.transform?.match(/translateX\((-?[\d.]+)px\)/)?.[1] || 0);
      const stride = viewport.clientWidth >= 900 ? viewport.clientWidth / 2 : viewport.clientWidth;
      return {left: 20 + 22 + Math.floor((starts[anchor.block] + anchor.char) / chars) * stride + translate};
    }}),
  };
  vm.createContext(ctx);
  vm.runInContext(source.slice(source.indexOf('function createReaderPages('), source.indexOf('function readerTextBlocks(')), ctx);
  vm.runInContext(source.slice(source.indexOf('function visibleReaderAnchor('), source.indexOf('function followReaderAnchor(')), ctx);
  vm.runInContext(source.slice(source.indexOf('async function moveReaderChapter('), source.indexOf('function voiceState(')), ctx);
  vm.runInContext(source.slice(source.indexOf('$("chapterSelect").onchange'), source.indexOf('function flushReadingPosition(')), ctx);
  const reader = ctx.state.reader;
  reader.pages = ctx.createReaderPages(viewport, article, blocks, ctx.anchorRange);
  const layout = reader.pages.layout;
  reader.pages.layout = (...args) => {layoutCalls++; return layout(...args);};
  reader.anchor = reader.pages.layout(null);
  ctx.loadChapter = async (chapter, offset) => {
    requests.push([chapter, offset]); reader.chapter = chapter;
    reader.anchor = reader.pages.layout(null, offset);
  };
  const target = {closest: () => null};
  const pointer = x => ({isPrimary: true, button: 0, clientX: 20 + 390 * x, clientY: 100, target});
  return {ctx, reader, nodes, viewport, requests, events, frames, pointer,
    reflow: n => {chars = n; ctx.repaginateReader();},
    tick: () => {for (const fn of frames.splice(0)) fn();},
    advance: n => {now += n;}, get saves() {return saveCount;}, get layouts() {return layoutCalls;}};
}

test('screen columns: exact page anchors inside paragraphs, bounds and legacy fraction', () => {
  const h = harness(), p = h.reader.pages;
  assert.equal(p.count, 5);
  assert.deepEqual({...p.firstAnchor()}, {block: 1, char: 0}, 'Whitespace-only nodes are not reading positions');
  for (let page = 0; page < 5; page++) {
    p.show(page);
    assert.equal(p.firstAnchor().char, page * 20);
    assert.equal(h.nodes.readerText.style.transform, `translateX(${-page * 390}px)`);
  }
  p.show(999); assert.equal(p.page, 4);
  p.show(-4); assert.equal(p.page, 0);
  p.layout(null, .5); assert.equal(p.page, 2);
  p.layout(null, 1); assert.equal(p.page, 4);
  p.layout({block: 1, char: 41}); assert.equal(p.page, 2);
  p.follow({block: 2, char: 1}); assert.equal(p.page, 4);
  p.layout({block: 999, char: 0}); assert.equal(p.page, 0);
});

test('tablet spreads retain individual audio pages and stable geometry on the right page', async () => {
  const h = harness(); h.viewport.clientWidth = 1100; h.reflow(20); h.tick();
  const p = h.reader.pages;
  assert.equal(p.columns, 2); assert.equal(p.count, 5);
  assert.equal(h.nodes.readerText.style.columnWidth, '506px');
  p.follow({block:1, char:21});
  assert.equal(p.page, 1); assert.equal(p.spreadStart, 0); assert.equal(p.spreadEnd, 1);
  assert.equal(h.nodes.readerText.style.transform, 'translateX(0px)');
  assert.equal(p.firstAnchor().char, 20, 'Save/start speech from the active right page');
  assert.equal(p.bounds().end.char, 40);
  await h.ctx.turnReaderPage(1); assert.equal(p.page, 2);
  assert.equal(h.reader.anchor.char, 40);
  assert.equal(h.nodes.readerText.style.transform, 'translateX(-1100px)');
  p.follow({block:1, char:61}); assert.equal(p.page, 3);
  assert.equal(p.firstAnchor().char, 60);
  await h.ctx.turnReaderPage(1); assert.equal(p.page, 4); assert.equal(p.spreadEnd, 4);
  await h.ctx.turnReaderPage(1); assert.equal(h.reader.chapter, 1);
  await h.ctx.turnReaderPage(-1); assert.equal(h.reader.chapter, 0); assert.equal(p.page, 4);
  await h.ctx.turnReaderPage(-1); assert.equal(p.page, 2);
  await h.ctx.turnReaderPage(-1); assert.equal(p.page, 0);
});

test('rotation between spread and phone preserves text anchors, counts and page labels', () => {
  const h = harness(); h.viewport.clientWidth = 1100; h.reflow(20); h.tick();
  h.reader.pages.follow({block:1, char:61}); h.reader.anchor = {block:1, char:61};
  h.reader.bookPages = {refresh() {}, position: (chapter, page) => ({page: page + 1, total: 5})};
  h.ctx.updateReaderPages(); assert.equal(h.nodes.readerPage.textContent, 'Стр. 3–4 / 5');
  h.viewport.clientWidth = 390; h.reflow(10); h.tick();
  assert.equal(h.reader.pages.columns, 1); assert.equal(h.reader.pages.page, 6);
  assert.equal(h.reader.anchor.char, 61);
  h.viewport.clientWidth = 1100; h.reflow(20); h.tick();
  assert.equal(h.reader.pages.page, 3); assert.equal(h.reader.anchor.char, 61);
});

test('speech page ranges are contiguous, measured and do not move the visible page', () => {
  const h = harness(), p = h.reader.pages;
  p.show(2);
  const bounds = Array.from({length:p.count}, (_, i) => p.bounds(i));
  assert.equal(p.page, 2);
  for (let i=0; i<bounds.length-1; i++) assert.deepEqual(bounds[i].end, bounds[i+1].anchor);
  assert.deepEqual({...bounds.at(-1).end}, {block:2, char:5});
  assert.equal(bounds[1].anchor.char, 20); assert.equal(bounds[1].end.char, 40);
});
test('reflow preserves character anchor and coalesces resize/font/panel changes', () => {
  const h = harness();
  h.reader.anchor = {block: 1, char: 41};
  h.reader.pages.follow(h.reader.anchor);
  h.reflow(10); h.ctx.repaginateReader(); h.ctx.repaginateReader();
  assert.equal(h.frames.length, 1);
  h.tick();
  assert.equal(h.reader.pages.page, 4);
  assert.equal(h.reader.pages.count, 9);
  assert.deepEqual({...h.reader.anchor}, {block: 1, char: 41});
  h.viewport.clientWidth = 700; h.reflow(30); h.tick();
  assert.equal(h.reader.pages.page, 1);
  assert.equal(h.reader.anchor.char, 41);
  h.ctx.repaginateReader(); h.ctx.state.reader = null; h.tick();
  assert.equal(h.layouts, 3, 'Closing cancels a queued layout');
});

test('opening speech pins the visible page start before the controls change page geometry', () => {
  const h = harness(); h.reader.id = '42';
  h.reader.pages.show(1); h.reader.anchor = {block: 1, char: 39};
  const start = h.ctx.readerSpeechStartPosition();
  assert.equal(start.anchor.char, 20); assert.equal(h.reader.anchor.char, 20);
  h.reflow(12); h.tick();
  assert.equal(h.reader.anchor.char, 20);
  assert.equal(h.reader.pages.page, 1, 'Reflow follows the chosen page start, not the old cue at character 39');
});
test('empty chapters have one page; hidden viewport does not overwrite an anchor', () => {
  const h = harness();
  const p = h.ctx.createReaderPages(h.viewport, {style: {}, scrollWidth: 346}, [], () => null);
  assert.deepEqual({...p.layout(null)}, {block: 0, char: 0});
  assert.equal(p.count, 1); assert.equal(p.offset, 0);
  h.viewport.clientWidth = 0;
  assert.deepEqual({...p.layout({block: 1, char: 21})}, {block: 1, char: 21});
});
test('page buttons cross chapter edges in both directions; TOC stays a chapter jump', async () => {
  const h = harness();
  await h.nodes.previousChapter.onclick(); assert.equal(h.requests.length, 0);
  for (let i = 0; i < 4; i++) await h.nodes.nextChapter.onclick();
  assert.equal(h.requests.length, 0); assert.equal(h.reader.pages.page, 4);
  await h.nodes.nextChapter.onclick();
  assert.deepEqual(h.requests, [[1, 0]]); assert.equal(h.reader.pages.page, 0);
  await h.nodes.previousChapter.onclick();
  assert.deepEqual(h.requests.at(-1), [0, 1]); assert.equal(h.reader.pages.page, 4);
  h.nodes.chapterSelect.value = '2'; await h.nodes.chapterSelect.onchange();
  assert.equal(h.reader.chapter, 2); assert.equal(h.reader.pages.page, 0);
  h.reader.pages.show(4); h.ctx.updateReaderPages();
  assert.equal(h.nodes.nextChapter.disabled, true);
  await h.nodes.nextChapter.onclick(); assert.equal(h.requests.length, 3);
});
test('iPhone column-width regression: a long chapter turns screen pages before chapter navigation', async () => {
  const h = harness({legacyWebkit: true});
  assert.equal(h.nodes.readerText.style.columnWidth, '346px');
  assert.equal(h.reader.pages.count, 5);
  for (let page = 1; page < 5; page++) {
    await h.nodes.nextChapter.onclick();
    assert.equal(h.reader.pages.page, page);
    assert.equal(h.reader.chapter, 0);
    assert.equal(h.requests.length, 0);
  }
  await h.nodes.nextChapter.onclick();
  assert.equal(h.reader.chapter, 1);
  h.viewport.clientWidth = 700; h.reflow(30); h.tick();
  assert.equal(h.nodes.readerText.style.columnWidth, '656px');
});
test('pending chapter navigation locks repeated taps and restores controls after failure', async () => {
  const h = harness();
  h.reader.pages.show(4);
  let finish;
  h.ctx.savePosition = () => new Promise(resolve => {finish = resolve;});
  const moving = h.ctx.turnReaderPage(1);
  assert.equal(h.nodes.nextChapter.disabled, true);
  await h.ctx.turnReaderPage(1); assert.equal(h.requests.length, 0);
  finish(); await moving; assert.equal(h.requests.length, 1);
  h.ctx.savePosition = async () => {};
  h.ctx.loadChapter = async () => {throw Error('offline');};
  h.nodes.chapterSelect.value = '2';
  await assert.rejects(h.nodes.chapterSelect.onchange(), /offline/);
  assert.equal(h.nodes.chapterSelect.value, 1);
  assert.equal(h.reader.navigating, false);
  assert.equal(h.nodes.nextChapter.disabled, false);
});
test('left/right taps turn; center, links, selection, drag, long press and cancellation do not', async () => {
  const h = harness(), v = h.viewport;
  async function tap(x) { const e = h.pointer(x); v.onpointerdown(e); await v.onclick(e); }
  await tap(.9); assert.equal(h.reader.pages.page, 1);
  await tap(.1); assert.equal(h.reader.pages.page, 0);
  await tap(.5); assert.equal(h.reader.pages.page, 0);
  let e = h.pointer(.9); e.target = {closest: () => ({})};
  v.onpointerdown(e); await v.onclick(e); assert.equal(h.reader.pages.page, 0);
  h.ctx.window.getSelection = () => ({isCollapsed: false});
  await tap(.9); assert.equal(h.reader.pages.page, 0);
  h.ctx.window.getSelection = () => ({isCollapsed: true});
  e = h.pointer(.9); v.onpointerdown(e); v.onpointermove({...e, clientX: 30});
  await v.onclick(e); assert.equal(h.reader.pages.page, 0);
  v.onpointerdown(e); h.advance(600); await v.onclick(e); assert.equal(h.reader.pages.page, 0);
  v.onpointerdown(e); v.onpointercancel(); await v.onclick(e); assert.equal(h.reader.pages.page, 0);
  let prevented = false;
  await h.events.keydown({key: 'ArrowRight', target: {closest: () => null}, preventDefault() {prevented = true;}});
  assert(prevented); assert.equal(h.reader.pages.page, 1);
  await h.events.keydown({key: 'ArrowRight', target: {closest: () => ({})}});
  assert.equal(h.reader.pages.page, 1, 'Keyboard use inside settings/TOC must not turn a page');
});
test('page turn moves speech to the new text and resumes only if previously playing', async () => {
  const h = harness(), spoken = [];
  h.ctx.readerVoice = {active: true, status: 'playing', pause() {this.status = 'paused';},
    play() {spoken.push({...h.reader.anchor}); this.status = 'playing';}};
  await h.ctx.turnReaderPage(1);
  assert.deepEqual(spoken, [{block: 1, char: 20}]);
  h.ctx.readerVoice.status = 'paused'; await h.ctx.turnReaderPage(1);
  assert.equal(spoken.length, 1); assert.equal(h.reader.anchor.char, 40);
});

test('real server voice: rapid manual pages replace preparing audio and ignore its old callbacks', async () => {
  const {ServerVoice} = require('../src/octofox_library/web/server-voice.js');
  const h = harness(), calls = []; let id = 0;
  h.reader.id = '42';
  class Audio {
    constructor() {this.currentTime = 0;}
    canPlayType() {return 'probably';}
    play() {return Promise.resolve();}
    pause() {this.onpause?.();}
    load() {}
    removeAttribute() {}
  }
  const env = {Audio, crypto: {randomUUID: () => String(++id)}, navigator: {}, setTimeout: () => 1, clearTimeout() {}};
  const position = () => ({book: '42', chapter: h.reader.chapter, anchor: {...h.reader.anchor}});
  const voice = h.ctx.readerVoice = new ServerVoice(async (url, data) => {
    calls.push({url, data}); return {state: 'preparing', segments: []};
  }, {position, startPosition: () => ({...position(), anchor: h.ctx.visibleReaderAnchor()}),
    state() {}, save() {}, follow(p, current) {
      if (!current()) return false;
      h.reader.anchor = p.anchor; h.reader.pages.follow(p.anchor); return true;
    }}, {available: true}, env);
  voice.play(); const oldAudio = voice.audio, latePlaying = oldAudio.onplaying, lateTime = oldAudio.ontimeupdate;
  oldAudio.onplaying();
  await h.ctx.turnReaderPage(1);
  assert.equal(voice.status, 'loading'); assert.equal(h.reader.pages.page, 1);
  await h.ctx.turnReaderPage(1);
  assert.equal(h.reader.pages.page, 2);
  assert.deepEqual(calls.filter(c => c.url.endsWith('/speech')).map(c => c.data.anchor.char), [0, 20, 40]);
  latePlaying(); lateTime();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(h.reader.pages.page, 2);
  voice.pause(); const before = calls.filter(c => c.url.endsWith('/speech')).length;
  await h.ctx.turnReaderPage(-1);
  assert.equal(h.reader.pages.page, 1); assert.equal(voice.audio, null);
  assert.equal(calls.filter(c => c.url.endsWith('/speech')).length, before);
  voice.play(); assert.equal(calls.filter(c => c.url.endsWith('/speech')).at(-1).data.anchor.char, 20);
  h.reader.pages.show(4); h.reader.anchor = h.ctx.visibleReaderAnchor();
  await h.ctx.turnReaderPage(1);
  assert.equal(h.reader.chapter, 1);
  const last = calls.filter(c => c.url.endsWith('/speech')).at(-1).data;
  assert.equal(last.chapter, 1); assert.equal(last.anchor.char, 0);
  voice.stop();
});

test('pagination CSS and markup retain separate accessible TOC and cache-busted assets', () => {
  const css = fs.readFileSync('src/octofox_library/web/styles.css', 'utf8');
  const html = fs.readFileSync('src/octofox_library/web/index.html', 'utf8');
  assert(css.includes('column-count: auto;') && css.includes('column-fill: auto;'));
  assert(source.includes('article.style.columnWidth = article.style.width;'));
  assert(css.includes('column-gap: 44px;') && css.includes('margin: 16px 22px;'));
  assert(html.includes('Предыдущая страница') && html.includes('Следующая страница'));
  const top = html.split('<header class="reader-top">')[1].split('</header>')[0];
  const bottom = html.split('<footer class="reader-bottom">')[1].split('</footer>')[0];
  assert(top.includes('id="chapterSelect" aria-label="Оглавление"'));
  assert(top.includes('class="icon-button reader-toc"'));
  assert(!bottom.includes('chapterSelect') && !bottom.includes('Оглавление'));
  assert(css.includes('.reader-toc select') && css.includes('inset: 0; width: 100%; height: 100%;'));
  assert(html.includes('/app.js?v=41') && html.includes('/styles.css?v=41'));
  assert(source.includes('new ResizeObserver(repaginateReader)'));
  assert(source.includes('document.fonts?.addEventListener("loadingdone", () =>'));
  assert(source.includes('state.reader?.bookPages?.invalidate()'));
  assert(css.includes('height: 100dvh;'));
  assert(css.includes('min-height: 0;') && css.includes('flex-shrink: 0;'));
});
