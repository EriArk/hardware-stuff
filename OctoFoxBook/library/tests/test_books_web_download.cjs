/* Exercise the shipped download handler without a browser or real account. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const {File} = require('node:buffer');
const source = fs.readFileSync('src/octofox_library/web/app.js', 'utf8');
const helpers = source.slice(source.indexOf('function standaloneApp('), source.indexOf('function renderBook('));
function fixture({standalone = true, cssStandalone = false, canShare = true} = {}) {
  const calls = {fetch: 0, share: 0, prevent: 0, login: 0};
  const ctx = {
    navigator: {standalone, canShare: () => canShare, share: async data => {
      calls.share++; calls.shared = data.files[0];
    }},
    window: {matchMedia: () => ({matches: cssStandalone})},
    File, AbortController, setTimeout, clearTimeout,
    showLogin: () => {calls.login++;},
    fetch: async (url, options) => {
      calls.fetch++; calls.options = options; calls.url = url;
      return {ok: true, headers: new Headers({'Content-Disposition':
        "attachment; filename*=UTF-8''%D0%A2%D0%B5%D0%BD%D0%B8.fb2"}),
        blob: async () => new Blob(['<FictionBook>test</FictionBook>'])};
    },
  };
  vm.createContext(ctx);
  vm.runInContext(helpers, ctx);
  const attributes = {};
  const link = {textContent: 'Скачать файл', href: 'https://books.test/reader-api/books/1/download',
    setAttribute: (key, value) => {attributes[key] = value;},
    removeAttribute: key => {delete attributes[key];}};
  const status = {hidden: true, textContent: ''};
  const cleanup = ctx.bindBookDownload(link, {id: '1', title: 'Запасное имя'}, status);
  const click = () => link.onclick({preventDefault: () => {calls.prevent++;}});
  return {ctx, link, status, calls, attributes, click, cleanup};
}
(async () => {
  const browser = fixture({standalone: false});
  await browser.click();
  assert.equal(browser.calls.prevent, 0);
  assert.equal(browser.calls.fetch, 0, 'Regular browser retains native download');

  const pwa = fixture();
  await pwa.click();
  assert.equal(pwa.calls.prevent, 1, 'PWA navigation is always intercepted');
  assert.equal(pwa.calls.fetch, 1);
  assert.equal(pwa.calls.options.credentials, 'same-origin');
  assert.equal(pwa.calls.options.redirect, 'error');
  assert.equal(pwa.calls.share, 0, 'No share after async fetch with expired activation');
  assert.equal(pwa.link.textContent, 'Сохранить / открыть…');
  assert.equal(pwa.status.hidden, false);
  const sharing = pwa.click();
  assert.equal(pwa.calls.share, 1, 'Share is invoked synchronously in the second tap');
  await sharing;
  assert.equal(pwa.calls.shared.name, 'Тени.fb2');
  assert.equal(await pwa.calls.shared.text(), '<FictionBook>test</FictionBook>');
  assert.equal(pwa.calls.fetch, 1);
  assert.deepEqual(pwa.attributes, {});

  pwa.ctx.navigator.share = async () => {throw {name: 'AbortError'};};
  await pwa.click();
  assert.match(pwa.status.textContent, /отменено/);
  assert.equal(pwa.link.textContent, 'Сохранить / открыть…');
  assert.equal(pwa.calls.fetch, 1, 'Cancellation keeps file for retry');
  pwa.ctx.navigator.share = async () => {throw {name: 'NotAllowedError'};};
  await pwa.click();
  assert.match(pwa.status.textContent, /ещё раз/);

  const unsupported = fixture({standalone: false, cssStandalone: true, canShare: false});
  await unsupported.click();
  assert.equal(unsupported.calls.prevent, 1);
  assert.match(unsupported.status.textContent, /Safari/);
  assert.equal(unsupported.calls.share, 0);
  const noApi = fixture();
  noApi.ctx.navigator.share = undefined;
  await noApi.click();
  assert.equal(noApi.calls.fetch, 0);
  assert.equal(noApi.calls.prevent, 1, 'Never fall through to the trapped file viewer');
  assert.match(noApi.status.textContent, /Safari/);

  const retry = fixture();
  const normalFetch = retry.ctx.fetch;
  retry.ctx.fetch = async () => {throw new Error('No connection');};
  await retry.click();
  assert.match(retry.status.textContent, /No connection/);
  assert.equal(retry.link.textContent, 'Скачать файл');
  retry.ctx.fetch = normalFetch;
  await retry.click();
  assert.equal(retry.link.textContent, 'Сохранить / открыть…');

  const expired = fixture();
  expired.ctx.fetch = async () => ({ok: false, status: 401});
  await expired.click();
  assert.equal(expired.calls.login, 1);
  assert.equal(expired.link.textContent, 'Скачать файл');
  const serverError = fixture();
  serverError.ctx.fetch = async () => ({ok: false, status: 429,
    json: async () => ({error: 'Попробуй позже'})});
  await serverError.click();
  assert.equal(serverError.status.textContent, 'Попробуй позже');

  const closing = fixture();
  let finish;
  closing.ctx.fetch = (url, options) => {
    closing.calls.fetch++;
    closing.calls.signal = options.signal;
    return new Promise(resolve => {finish = resolve;});
  };
  const pending = closing.click();
  await closing.click();
  assert.equal(closing.calls.fetch, 1, 'Ignore duplicate taps');
  closing.cleanup();
  assert(closing.calls.signal.aborted, 'Closing the card aborts download');
  finish({ok: true, headers: new Headers(), blob: async () => new Blob(['fb2'])});
  await pending;
  assert.equal(closing.calls.share, 0);
  assert(!closing.status.textContent.includes('Файл готов'), 'No stale ready state after closing');

  assert.equal(pwa.ctx.downloadFilename(null, '../Книга<1>.fb2'), '.._Книга_1_.fb2');
  assert.equal(pwa.ctx.downloadFilename("filename*=UTF-8''%broken", 'Книга'), 'Книга.fb2');
  assert(source.includes('releaseBookDownload();'), 'Card lifecycle releases file references');
  console.log('PWA download: two-tap share, cancellation, retry, errors, cleanup and browser fallback OK');
})().catch(error => {console.error(error); process.exitCode = 1;});
