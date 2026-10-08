/* Real playback controller/backend with a deterministic speech engine. */
const assert = require('node:assert/strict');
const {chunks, VoiceController, BrowserSpeech} = require('../src/octofox_library/web/voice-reader.js');
const flush = () => new Promise(resolve => setImmediate(resolve));
function fixture(chapters = ['Первая фраза. Вторая фраза.', '', 'Последняя глава.']) {
  let chapter = 0, position = {block: 0, char: 0}, finished = 0, saved = 0;
  const spoken = [], statuses = [], positions = [];
  const backend = {available: true, cancel() {}, speak(text, events) {spoken.push({text, events});}};
  const view = {
    position: () => position,
    segments: () => chunks(chapters[chapter], 0, 16),
    follow: p => {position = {...p}; positions.push(p);},
    state: (...args) => statuses.push(args), save: () => saved++,
    next: async current => {
      if (!current() || chapter + 1 >= chapters.length) return false;
      chapter++; position = {block: 0, char: 0}; return true;
    }, finish: async () => finished++,
  };
  const player = new VoiceController(backend, view);
  return {player, view, backend, spoken, statuses, positions,
    current: () => ({chapter, position, finished, saved}), last: () => spoken.at(-1)};
}
(async () => {
  const text = '  Привет!\n\n' + 'Длинное слово и текст. '.repeat(100) + ' 😀 Конец.';
  const list = chunks(text, 3);
  assert(list.every(s => s.end > s.start && s.text.length <= 220 && s.block === 3));
  assert.equal(list.map(s => s.text).join('').replace(/\s/g, ''), text.replace(/\s/g, ''));
  assert.deepEqual(chunks(' \n\t', 0), []);
  assert.deepEqual(chunks('Я', 0).map(s => s.text), ['Я']);
  let f = fixture();
  f.view.follow({block: 0, char: 7});
  f.player.play();
  assert(f.last().text.startsWith('фраза'), 'Start at saved character, not chapter start');
  f.last().events.boundary(2);
  const beforePause = {...f.current().position};
  const stale = f.last();
  f.player.pause();
  assert.equal(f.player.status, 'paused');
  stale.events.end();
  stale.events.boundary(8);
  assert.deepEqual(f.current().position, beforePause, 'Stale callbacks cannot change progress');
  f.player.play();
  assert.equal(f.current().position.char, beforePause.char);
  for (let i = 0; i < 30 && f.player.status !== 'ended'; i++) {
    if (f.player.status === 'playing') f.last().events.end();
    await flush();
  }
  assert.equal(f.player.status, 'ended');
  assert.equal(f.current().finished, 1);
  assert.equal(f.current().chapter, 2, 'Continuous passage crosses empty chapter');

  f = fixture(); f.player.play(); const old = f.last();
  f.player.seek({block: 0, char: 14});
  old.events.end(); old.events.error('failure');
  assert.equal(f.current().position.char, 14);
  assert.equal(f.player.status, 'playing');
  f.player.pause(); f.player.seek({block: 0, char: 0});
  assert.equal(f.player.status, 'paused', 'Seeking while paused stays paused');
  f.player.play(); f.last().events.error('voice-unavailable');
  assert.equal(f.player.status, 'error');
  assert.equal(f.statuses.at(-1)[1], 'voice-unavailable');
  f.player.play(); assert.equal(f.player.status, 'playing');
  const stopping = f.last(); f.player.stop(); stopping.events.end();
  assert.equal(f.player.status, 'idle');

  f = fixture(['']); let resolveNext;
  f.view.next = () => new Promise(resolve => {resolveNext = resolve;});
  f.player.play(); assert.equal(f.player.status, 'loading');
  f.player.stop(); resolveNext(true); await flush();
  assert.equal(f.player.status, 'idle', 'Stop during chapter fetch never restarts');
  f = fixture(['']); f.view.next = async () => {throw Error('offline');};
  f.player.play(); await flush();
  assert.equal(f.player.status, 'error');
  assert.equal(f.statuses.at(-1)[1], 'load-failed');
  f = fixture(); f.backend.available = false; f.player.play();
  assert.equal(f.spoken.length, 0); assert.equal(f.player.status, 'error');

  // Browser-specific backend is independently replaceable and cancel-safe.
  const voices = [{voiceURI: 'ru', lang: 'ru-RU', localService: true}];
  const utterances = [];
  const env = {SpeechSynthesisUtterance: class {constructor(text) {this.text = text;}},
    speechSynthesis: {getVoices: () => voices, cancel() {}, speak(u) {utterances.push(u); u.onstart();}}};
  const browser = new BrowserSpeech(env); let ended = 0, boundary = -1;
  browser.rate = 1.5;
  browser.speak('Текст', {end: () => ended++, boundary: n => boundary = n, error: assert.fail});
  const u = utterances[0], lateEnd = u.onend;
  assert.equal(u.rate, 1.5); assert.equal(u.voice.voiceURI, 'ru');
  u.onboundary({charIndex: 3}); assert.equal(boundary, 3);
  browser.cancel(); lateEnd(); assert.equal(ended, 0);
  assert.equal(u.onend, null);
  const unsupported = new BrowserSpeech({});
  let unavailable;
  unsupported.speak('x', {error: e => unavailable = e});
  assert.equal(unavailable, 'unavailable');
  console.log('Voice: chunking, current position, continuous/empty chapters, pause/resume, seeking, completion, stale events, cancellation, retries and backend isolation OK');
})().catch(e => {console.error(e); process.exitCode = 1;});
