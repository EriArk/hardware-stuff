const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('src/octofox_library/web/app.js', 'utf8');

function harness() {
  const events = {}, calls = [];
  const doc = {activeElement: null, addEventListener(name, fn, capture) {
    events[name] = {fn, capture};
  }};
  const nodes = {};
  for (const id of ['voiceControls', 'voiceEngine', 'voiceHint', 'voiceStatus', 'voicePreparing',
    'voicePlay', 'listenBook', 'closeVoiceControls', 'readerScroll', 'drawer']) {
    nodes[id] = {id, hidden: id === 'voiceControls', attrs: {},
      setAttribute(name, value) {this.attrs[name] = value;},
      contains(target) {return target === this || target?.parent === this;},
      focus(options) {doc.activeElement = this; this.focusOptions = options;},
      classList: {contains: () => false},
    };
  }
  nodes.voiceEngine.parent = nodes.voiceControls;
  nodes.closeVoiceControls.parent = nodes.voiceControls;
  const reader = {anchor: {block: 10, char: 30}, pages: {page: 5, count: 100}};
  const voice = {status: 'playing', engine: 'server',
    play() {calls.push('play'); this.status = 'playing';},
    pause() {calls.push('pause'); this.status = 'paused';},
    stop() {calls.push('stop'); this.status = 'idle';}};
  const ctx = {$: id => nodes[id], document: doc, state: {reader}, readerVoice: voice, navigator: {},
    configureVoice() {calls.push('configure');}, readerPointer: {x: 30},
    setDrawer() {}, closeReader: async () => {calls.push('closeReader');}, toast() {},
    repaginateReader() {throw Error('Popover must not repaginate');},
    savePosition() {throw Error('Popover must not write a new position');}};
  vm.createContext(ctx);
  vm.runInContext(source.slice(source.indexOf('function voiceState('), source.indexOf('function configureVoice(')), ctx);
  vm.runInContext(source.slice(source.indexOf('function toggleVoice('), source.indexOf('function updateVoiceRateLabel(')), ctx);
  vm.runInContext(source.slice(source.indexOf('document.addEventListener("keydown", (event) => {'), source.indexOf('let touch = null,')), ctx);
  return {ctx, nodes, reader, voice, calls, events, doc};
}

test('audio popover is out of flow, with separate icon-only playback in the header', () => {
  const html = fs.readFileSync('src/octofox_library/web/index.html', 'utf8');
  const css = fs.readFileSync('src/octofox_library/web/styles.css', 'utf8');
  const header = html.match(/<header class="reader-top">([\s\S]*?)<\/header>/)[1];
  const panel = header.match(/<section id="voiceControls"([\s\S]*?)<\/section>/)[0];
  assert(!panel.includes('id="voicePlay"'));
  assert(!html.includes('id="voiceStop"'));
  assert(!source.includes('$("voiceStop")'));
  for (const id of ['voicePlay', 'listenBook']) {
    const button = header.match(new RegExp(`<button id="${id}"[\\s\\S]*?</button>`))[0];
    assert(button.includes('<svg'));
    assert(!button.replace(/<[^>]+>/g, '').trim());
    assert.equal((html.match(new RegExp(`id="${id}"`, 'g')) || []).length, 1);
  }
  assert(panel.includes('role="dialog"') && panel.includes('aria-modal="false"'));
  assert(header.includes('aria-controls="voiceControls"') && header.includes('aria-haspopup="dialog"'));
  const rule = css.match(/\.voice-controls\s*\{([^}]+)\}/)[1];
  assert(rule.includes('position: absolute') && rule.includes('top: calc(100% + 8px)'));
  assert(rule.includes('overflow-y: auto') && rule.includes('100dvh') && rule.includes('safe-area-inset-bottom'));
  assert(css.match(/\.reader-top\s*\{[^}]*position: relative/));
  assert(css.match(/\.reader-top > span\s*\{[^}]*min-width: 0/));
});

test('open/close does not play, pause, save, reflow or change the current page', () => {
  const h = harness(), before = JSON.stringify(h.reader);
  h.nodes.voiceStatus.textContent = 'An existing audio error';
  h.nodes.listenBook.onclick();
  assert.equal(h.nodes.voiceControls.hidden, false);
  assert.equal(h.nodes.listenBook.attrs['aria-expanded'], 'true');
  assert.equal(h.doc.activeElement, h.nodes.closeVoiceControls);
  assert.equal(h.nodes.closeVoiceControls.focusOptions.preventScroll, true);
  assert.equal(h.nodes.voiceEngine.focusOptions, undefined, 'Do not activate the native iPhone picker');
  h.nodes.closeVoiceControls.onclick();
  assert.equal(h.nodes.voiceControls.hidden, true);
  assert.equal(h.doc.activeElement, h.nodes.listenBook);
  assert.deepEqual(h.calls, ['configure']);
  assert.equal(JSON.stringify(h.reader), before);
  assert.equal(h.nodes.voiceStatus.textContent, 'An existing audio error');
});

test('speech updates do not open/close the panel; top button controls play and pause independently', () => {
  const h = harness();
  for (const hidden of [true, false]) {
    h.nodes.voiceControls.hidden = hidden;
    for (const status of ['idle', 'loading', 'playing', 'paused', 'error', 'ended']) {
      h.ctx.voiceState(status);
      assert.equal(h.nodes.voiceControls.hidden, hidden);
      assert.equal(h.nodes.voicePlay.disabled, false, 'Loading can be paused');
      assert.equal(h.nodes.voicePlay.attrs['aria-pressed'], String(['loading', 'playing'].includes(status)));
    }
  }
  h.nodes.voiceControls.hidden = true;
  h.nodes.voicePlay.onclick();
  assert.equal(h.voice.status, 'paused');
  h.nodes.voicePlay.onclick();
  assert.equal(h.voice.status, 'playing');
  h.voice.status = 'loading'; h.nodes.voicePlay.onclick();
  assert.equal(h.voice.status, 'paused');
  assert.equal(h.nodes.voiceControls.hidden, true);
  h.ctx.state.reader.loading = true;
  const n = h.calls.length; h.nodes.voicePlay.onclick();
  assert.equal(h.calls.length, n);
});

test('outside tap dismisses without turning the page; header playback stays clickable', () => {
  const h = harness();
  const click = target => {
    const event = {target, prevented: false, stopped: false,
      preventDefault() {this.prevented = true;}, stopPropagation() {this.stopped = true;}};
    h.events.click.fn(event); return event;
  };
  assert.equal(h.events.click.capture, true);
  h.ctx.setVoicePopover(true);
  click(h.nodes.voiceEngine);
  assert.equal(h.nodes.voiceControls.hidden, false);
  click(h.nodes.listenBook);
  assert.equal(h.nodes.voiceControls.hidden, false);
  const tap = click({parent: h.nodes.readerScroll});
  assert(tap.prevented && tap.stopped);
  assert.equal(h.ctx.readerPointer, null);
  assert.equal(h.nodes.voiceControls.hidden, true);
  h.ctx.setVoicePopover(true);
  const play = click(h.nodes.voicePlay);
  assert(!play.prevented && !play.stopped);
  assert.equal(h.nodes.voiceControls.hidden, true);
  h.nodes.voicePlay.onclick();
  assert.equal(h.voice.status, 'paused');
});

test('Escape closes only audio settings first and returns focus, not the book', () => {
  const h = harness(); h.ctx.setVoicePopover(true);
  let prevented = false;
  h.events.keydown.fn({key: 'Escape', preventDefault() {prevented = true;}});
  assert(prevented);
  assert.equal(h.nodes.voiceControls.hidden, true);
  assert.equal(h.doc.activeElement, h.nodes.listenBook);
  assert(!h.calls.includes('closeReader'));
  h.events.keydown.fn({key: 'Escape'});
  assert(h.calls.includes('closeReader'));
});
