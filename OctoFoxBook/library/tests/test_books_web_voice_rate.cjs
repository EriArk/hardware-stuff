const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const {BrowserSpeech} = require('../src/octofox_library/web/voice-reader.js');
test('speed slider labels drag, saves on release, and does not restart server narration', () => {
  const html = fs.readFileSync('src/octofox_library/web/index.html', 'utf8');
  const source = fs.readFileSync('src/octofox_library/web/app.js', 'utf8');
  assert(html.includes('id="voiceRate" type="range" min="0.5" max="2" step="0.05" value="0.85"'));
  assert.equal(new BrowserSpeech({}).rate, 0.85);
  const nodes = {voiceRate: {value: '0.85', setAttribute() {}}, voiceRateValue: {}, voiceSelect: {value: 'eugene'}};
  const saved = [], rates = []; let played = 0;
  const voice = {engine: 'server', status: 'playing', backend: {rate: .85, voiceURI: 'eugene'},
    setRate: rate => {rates.push(rate); voice.backend.rate = rate;}, play: () => played++};
  const ctx = {$: id => nodes[id], readerVoice: voice, localStorage: {setItem: (...args) => saved.push(args)}};
  vm.createContext(ctx);
  vm.runInContext(source.slice(source.indexOf('function updateVoiceRateLabel('), source.indexOf('$("voiceEngine").onchange')), ctx);
  nodes.voiceRate.value = '0.9'; nodes.voiceRate.oninput();
  assert.equal(nodes.voiceRateValue.textContent, '0,90×');
  assert.equal(rates.length, 0); assert.equal(saved.length, 0);
  nodes.voiceRate.onchange();
  assert.deepEqual(rates, [.9]); assert.equal(played, 0);
  assert.deepEqual(JSON.parse(saved[0][1]), {rate: .9, voice: 'eugene'});
  voice.engine = 'browser'; nodes.voiceRate.value = '1.05'; nodes.voiceRate.onchange();
  assert.equal(played, 1); assert.equal(voice.backend.rate, 1.05);
});
