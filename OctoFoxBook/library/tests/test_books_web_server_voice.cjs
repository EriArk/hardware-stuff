const {test} = require('node:test');
const assert = require('node:assert/strict');
const {ServerVoice} = require('../src/octofox_library/web/server-voice.js');
const fs = require('node:fs');

function fixture(native = true) {
  const calls = [], states = [], saved = [], finished = [], timers = new Map(); let n = 0;
  let position = {book: '42', chapter: 0, anchor: {block: 0, char: 0}};
  const info = {state: 'preparing', segments: []};
  class Audio {
    constructor() { this.currentTime = 0; this.playCount = 0; }
    canPlayType() { return native ? 'probably' : ''; }
    play() { this.playCount++; return Promise.resolve(); }
    pause() { this.onpause?.(); }
    removeAttribute() { this.src = ''; }
    load() { this.loadCount = (this.loadCount || 0) + 1; }
  }
  const env = {Audio, crypto: {randomUUID: () => `id-${++n}`}, navigator: {audioSession: {type: 'auto'}},
    setTimeout: cb => { const id = ++n; timers.set(id, cb); return id; },
    clearTimeout: id => timers.delete(id)};
  const api = async (url, data) => { calls.push({url, data}); return url.endsWith('/status') ? structuredClone(info) : {}; };
  const voice = new ServerVoice(api, {position: () => position, state: (s, e) => states.push([s, e]),
    follow: p => { position = p; }, save: p => saved.push(p), finish: p => finished.push(p)},
    {available: true, voices: [{voiceURI: 'eugene', lang: 'ru-RU'}]}, env);
  return {voice, env, calls, states, saved, finished, info, position: () => position,
    tick: async () => { await new Promise(resolve => setImmediate(resolve)); }};
}
function ready(f) {
  Object.assign(f.info, {state: 'ready', seek: 0, complete: false,
    segments: [{index: 0, duration: 10, at: 0, chapter: 0,
      cues: [{start: 0, anchor: {block: 0, char: 0}}, {start: 5, anchor: {block: 1, char: 0}}]}]});
}
test('native playback starts in original click; POST protects registration', async () => {
  const f = fixture(); f.voice.play();
  assert.equal(f.voice.audio.playCount, 1);
  assert.match(f.voice.audio.src, /audio.m3u8$/);
  assert.equal(f.calls[0].url, '/books/42/speech');
  assert.equal(f.calls[0].data.voice, 'eugene');
  assert.equal(f.voice.status, 'loading');
  await f.tick(); f.voice.stop();
  assert.equal(f.env.navigator.audioSession.type, 'auto');
});
test('ready playback follows sentence anchor and resumes without new preparation', async () => {
  const f = fixture(); ready(f); f.voice.play(); await f.tick();
  f.voice.audio.onplaying(); f.voice.audio.currentTime = 6; f.voice.audio.ontimeupdate();
  assert.equal(f.position().anchor.block, 1);
  f.voice.pause(); const audio = f.voice.audio; f.voice.play();
  assert.equal(f.voice.audio, audio);
  assert.equal(f.calls.filter(c => c.url.endsWith('/speech')).length, 1);
  f.voice.stop();
});
test('wav fallback stays on page and starts next ready fragment', async () => {
  const f = fixture(false); ready(f); f.voice.play(); await f.tick();
  assert.match(f.voice.audio.src, /0.wav$/);
  f.info.segments.push({...f.info.segments[0], index: 1, at: 10});
  f.voice.audio.onended(); await f.tick();
  assert.match(f.voice.audio.src, /1.wav$/); f.voice.stop();
});
test('changing voice gets a fresh stream, stop does not rewind reading position', async () => {
  const f = fixture(); ready(f); f.voice.play(); await f.tick();
  const old = f.voice.id; f.voice.backend.voiceURI = 'kseniya'; f.voice.play(); await f.tick();
  assert.notEqual(f.voice.id, old);
  assert(f.calls.some(c => c.url === `/speech/streams/${old}/stop`));
  assert(f.calls.some(c => c.data?.voice === 'kseniya'));
  const before = f.position(); f.voice.stop(); assert.deepEqual(f.position(), before);
});
test('late status response cannot restart stopped playback', async () => {
  const f = fixture(); f.voice.play(); f.voice.stop(); await f.tick();
  assert.equal(f.voice.audio, null); assert.equal(f.voice.status, 'idle');
});
test('repeated media errors end in actionable error instead of endless preparation', async () => {
  const f = fixture(); f.voice.play(); await f.tick();
  for (let i = 0; i < 4; i++) f.voice.audio.onerror();
  assert.equal(f.voice.status, 'error'); assert.equal(f.voice.audio, null);
});
test('native startup pins a new anchor-trimmed stream to zero instead of its live edge', async () => {
  const f = fixture(); f.voice.play();
  f.voice.audio.currentTime = 18;
  f.voice.audio.onloadedmetadata();
  assert.equal(f.voice.audio.currentTime, 0);
  f.voice.stop(); await f.tick();
});
test('only Russian server choices; preparation text and engine chooser shipped', () => {
  const app = fs.readFileSync('src/octofox_library/web/app.js', 'utf8');
  const html = fs.readFileSync('src/octofox_library/web/index.html', 'utf8');
  assert(app.includes('Подготовка озвучки. Ожидайте'));
  assert(html.includes('id="voiceEngine"'));
  assert(html.includes('/server-voice.js?v=5'));
  assert(html.includes('id="voicePreparing"') && html.includes('Аудиокнига подготавливается'));
  assert(html.includes('value="server"') && html.includes('value="browser"'));
  const choices = app.split('let serverSpeechInfo =')[1].split(']};')[0];
  assert.equal((choices.match(/ru-RU/g) || []).length, 2);
  assert(!choices.includes('kseniya'));
  assert(!choices.includes('en-'));
});
test('0.85 default and changing speed preserve native audio, stream, time and pitch', async () => {
  const f = fixture();
  assert.equal(f.voice.backend.rate, 0.85);
  f.voice.play(); await f.tick();
  const audio = f.voice.audio, stream = f.voice.id, calls = f.calls.length;
  audio.currentTime = 9;
  f.voice.setRate(0.9);
  assert.equal(audio.playbackRate, 0.9); assert.equal(audio.currentTime, 9);
  assert.equal(audio.preservesPitch, true);
  assert.equal(f.voice.id, stream); assert.equal(f.voice.audio, audio);
  assert.equal(f.calls.length, calls);
  f.voice.setRate(NaN); f.voice.setRate(99);
  assert.equal(audio.playbackRate, 0.9);
  f.voice.stop();
});

test('multiple page anchors and chapters follow one uninterrupted native audio source', async () => {
  const f = fixture(); ready(f);
  f.info.segments.push({index: 1, at: 10, duration: 10, chapter: 1,
    cues: [{start: 0, anchor: {block: 0, char: 0}}, {start: 5, anchor: {block: 1, char: 40}}]});
  f.voice.play(); await f.tick(); const audio = f.voice.audio, src = audio.src;
  audio.onplaying();
  for (const [seconds, chapter, block] of [[6, 0, 1], [11, 1, 0], [16, 1, 1]]) {
    audio.currentTime = seconds; audio.ontimeupdate(); await f.tick();
    assert.equal(f.position().chapter, chapter); assert.equal(f.position().anchor.block, block);
    assert.deepEqual(f.saved.at(-1), f.position());
    assert.equal(f.voice.audio, audio); assert.equal(audio.src, src);
    assert.equal(audio.playCount, 1); assert.equal(audio.loadCount || 0, 0);
  }
  f.voice.pause(); assert.deepEqual(f.saved.at(-1), f.position()); f.voice.stop();
});

test('temporary end waits for more audio and reloads at the audible time, never marks book read', async () => {
  const f = fixture(); ready(f); f.voice.play(); await f.tick();
  const audio = f.voice.audio; audio.onplaying(); audio.currentTime = 10;
  audio.onended(); await f.tick();
  assert.equal(f.voice.status, 'loading'); assert.equal(f.finished.length, 0);
  f.info.segments.push({...f.info.segments[0], index: 1, at: 10});
  await f.voice.poll(f.voice.epoch);
  assert.equal(audio.loadCount, 1);
  audio.currentTime = 0; audio.onloadedmetadata();
  assert.equal(audio.currentTime, 10); assert.equal(f.finished.length, 0);
  audio.onplaying(); assert.equal(f.voice.status, 'playing'); f.voice.stop();
});

test('only a fully generated book ending finishes and saves its final anchor', async () => {
  const f = fixture(); ready(f); f.info.complete = true;
  f.info.segments[0].cues[1].endAnchor = {block: 1, char: 130};
  f.voice.play(); await f.tick(); const audio = f.voice.audio;
  audio.onplaying(); audio.currentTime = 10; audio.onended(); await f.tick();
  assert.equal(f.voice.status, 'ended'); assert.equal(f.finished.length, 1);
  assert.deepEqual(f.saved.at(-1).anchor, {block: 1, char: 130}); f.voice.stop();
});

test('permanently failed next clip becomes retryable error even when native audio only waits', async () => {
  const f = fixture(); ready(f); f.info.blocked = {failed: true, index: 1};
  f.voice.play(); await f.tick(); const audio = f.voice.audio;
  audio.onplaying(); audio.currentTime = 9.9; audio.ontimeupdate(); await f.tick();
  audio.onwaiting(); await f.voice.poll(f.voice.epoch);
  assert.equal(f.voice.status, 'error'); assert.equal(f.finished.length, 0);
  assert.deepEqual(f.saved.at(-1).anchor, {block: 1, char: 0});
  assert.match(f.states.at(-1)[1], /Позиция сохранена/);
});

test('busy chapter view retries same cue and saves audible position before layout is ready', async () => {
  const f = fixture(); ready(f); f.voice.play(); await f.tick();
  const applied = f.voice.view.follow; let busy = true, attempts = 0;
  f.voice.view.follow = async (p, current) => { attempts++; return !busy && current() ? applied(p) : false; };
  const audio = f.voice.audio; audio.onplaying(); audio.currentTime = 6;
  audio.ontimeupdate(); await f.tick();
  assert.equal(f.position().anchor.block, 0); assert.equal(f.saved.at(-1).anchor.block, 1);
  busy = false; audio.ontimeupdate(); await f.tick();
  assert.equal(f.position().anchor.block, 1); assert.equal(attempts, 2);
  f.voice.stop();
});

test('late chapter completion after stop cannot change displayed position', async () => {
  const f = fixture(); ready(f); f.voice.play(); await f.tick();
  const applied = f.voice.view.follow; let release;
  f.voice.view.follow = async (p, current) => {
    await new Promise(resolve => release = resolve);
    return current() ? applied(p) : false;
  };
  const audio = f.voice.audio; audio.onplaying(); audio.currentTime = 6; audio.ontimeupdate();
  f.voice.stop(); release(); await f.tick();
  assert.equal(f.position().anchor.block, 0); assert.equal(f.saved.at(-1).anchor.block, 1);
});

test('one failed status request does not interrupt playing audio', async () => {
  const f = fixture(); ready(f); f.voice.play(); await f.tick();
  const audio = f.voice.audio; audio.onplaying();
  const api = f.voice.api; let errors = 1;
  f.voice.api = (...args) => errors-- > 0 ? Promise.reject(new Error('offline')) : api(...args);
  await f.voice.poll(f.voice.epoch); assert.equal(f.voice.audio, audio);
  await f.voice.poll(f.voice.epoch); assert.equal(f.voice.statusFailures, 0); f.voice.stop();
});

test('fresh playback uses visible page start, not a stale stored speech anchor', async () => {
  const f = fixture();
  f.voice.view.startPosition = () => ({book: '42', chapter: 2, anchor: {block: 7, char: 300}});
  f.voice.play(); await f.tick();
  const request = f.calls.find(c => c.url.endsWith('/speech'));
  assert.equal(request.data.chapter, 2);
  assert.deepEqual(request.data.anchor, {block: 7, char: 300});
  f.voice.stop();
});

test('manual navigation cancels pending old follow and late events before selecting the new page', async () => {
  const f = fixture(); ready(f); f.voice.play(); await f.tick();
  const old = f.voice.audio, applied = f.voice.view.follow; let release;
  f.voice.view.follow = async (p, current) => {
    await new Promise(resolve => release = resolve);
    return current() ? applied(p) : false;
  };
  old.onplaying(); old.currentTime = 6; old.ontimeupdate();
  const latePlaying = old.onplaying, lateTime = old.ontimeupdate;
  assert.equal(f.voice.navigate(), true);
  applied({book: '42', chapter: 1, anchor: {block: 3, char: 44}});
  const saves = f.saved.length;
  release(); latePlaying(); lateTime(); await f.tick();
  assert.equal(f.position().chapter, 1); assert.equal(f.position().anchor.char, 44);
  assert.equal(f.saved.length, saves, 'Cancelled audio does not save its old cue over the reader choice');
  f.voice.play(); await f.tick();
  assert.deepEqual(f.calls.filter(c => c.url.endsWith('/speech')).at(-1).data.anchor, {block: 3, char: 44});
  assert.equal(f.voice.spokenPosition, null); f.voice.stop();
});

test('navigation during preparation cancels that stream and preserves the intention to listen', async () => {
  const f = fixture(); f.voice.play(); const previous = f.voice.id;
  assert.equal(f.voice.status, 'loading'); assert.equal(f.voice.navigate(), true);
  f.voice.view.follow({book: '42', chapter: 0, anchor: {block: 5, char: 70}});
  f.voice.play(); await f.tick();
  assert.notEqual(f.voice.id, previous);
  assert.deepEqual(f.calls.filter(c => c.url.endsWith('/speech')).at(-1).data.anchor, {block: 5, char: 70});
  f.voice.pause(); assert.equal(f.voice.navigate(), false); f.voice.stop();
});

test('late playing event cannot unpause or follow old audio', async () => {
  const f = fixture(); ready(f); f.voice.play(); await f.tick();
  const audio = f.voice.audio; audio.onplaying(); f.voice.pause();
  f.voice.view.follow({book: '42', chapter: 0, anchor: {block: 8, char: 40}});
  audio.currentTime = 6; audio.onplaying(); audio.ontimeupdate(); await f.tick();
  assert.equal(f.voice.status, 'paused'); assert.equal(f.position().anchor.block, 8); f.voice.stop();
});

test('WAV pause and resume do not rewind to the clip beginning', async () => {
  const f = fixture(false); ready(f); f.voice.play(); await f.tick();
  const audio = f.voice.audio; audio.onloadedmetadata(); audio.onplaying();
  audio.currentTime = 6; audio.ontimeupdate(); await f.tick(); f.voice.pause();
  f.voice.play(); audio.onplaying();
  assert.equal(f.voice.audio, audio); assert.equal(audio.currentTime, 6); f.voice.stop();
});
