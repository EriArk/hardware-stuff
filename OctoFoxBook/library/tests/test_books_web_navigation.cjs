/* Real app handlers + deterministic clock; no browser or live account required. */
const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('src/octofox_library/web/app.js', 'utf8');

function harness() {
  const elements = new Map(), events = {}, requests = [], timers = new Map();
  const requestDetails = [], responses = {};
  let now = 0, timerId = 0, catalogStatus = 'ready', revision = 10;
  const make = () => ({
    value: '', hidden: false, disabled: false, open: false, children: [], replacements: 0,
    dataset: {}, scrollTop: 0, scrollLeft: 0, textContent: '',
    classList: {add() {}, remove() {}, toggle() {}, contains() {return false;}},
    setAttribute() {}, addEventListener() {}, focus() {}, contains() {return false;},
    querySelectorAll() {return [];}, querySelector() {return make();}, closest() {return make();},
    append(node) {this.children.push(...(node.fragment ? node.children : [node]));},
    replaceChildren() {this.children = []; this.replacements++;},
    insertAdjacentHTML() {}, close() {this.open = false;}, showModal() {this.open = true;},
    get childElementCount() {return this.children.length;},
  });
  const $ = id => {
    if (!elements.has(id)) elements.set(id, make());
    return elements.get(id);
  };
  $('sortFilter').value = 'recent'; $('searchScope').value = 'all';
  $('readerScreen').hidden = true;
  const shortcuts = ['genre', 'tag', 'author', 'series'].map(kind => {
    const b = make(); b.dataset.browse = kind; b.closest = () => null; return b;
  });
  const pickers = shortcuts.map(b => {
    const p = make(); p.dataset.browse = b.dataset.browse; p.closest = () => $('filters'); return p;
  });
  const schedule = (fn, ms, interval = false) => {
    timers.set(++timerId, {fn, ms, due: now + ms, interval}); return timerId;
  };
  const context = {
    console, URLSearchParams, Date, navigator: {}, localStorage: {getItem() {return null;}},
    BookBookmarks: require('../src/octofox_library/web/bookmarks.js'),
    location: {hash: ''},
    document: {hidden: false, activeElement: null, body: make(), getElementById: $,
      querySelectorAll: selector => selector === '[data-browse]' ? [...shortcuts, ...pickers] : [],
      addEventListener(name, fn) {(events[name] ||= []).push(fn);},
      createElement: make, createDocumentFragment() {return {...make(), fragment: true};}},
    window: {BookPersonal: {uploadBatch: require('../src/octofox_library/web/personal-library.js').uploadBatch, createPersonalLibrary: () => null}, scrollY: 0, scrollTo(x, y) {this.scrollY = y;}, addEventListener() {}},
    setTimeout: (fn, ms) => schedule(fn, ms), clearTimeout: id => timers.delete(id),
    setInterval: (fn, ms) => schedule(fn, ms, true), clearInterval: id => timers.delete(id),
    fetch: async (path, options) => {
      requests.push(path);
      requestDetails.push({path, options});
      let value;
      if (path in responses) value = responses[path];
      else if (path.startsWith('/reader-api/books?')) {
        const page = new URLSearchParams(path.split('?')[1]).get('page');
        value = {books: [{id: page, title: `Book ${page}`, author: 'Author', cover: '/cover'}],
          total: 100, more: true, indexStatus: catalogStatus, catalogRevision: revision};
      } else if (path.startsWith('/reader-api/facets?'))
        value = {groups: [], letters: [], items: [], total: 0, more: false};
      else if (path === '/reader-api/catalog-status') value = {status: catalogStatus, revision};
      else value = [];
      return {ok: true, json: async () => value};
    },
  };
  vm.createContext(context);
  vm.runInContext(source.slice(0, source.lastIndexOf('api("/me")')), context);
  const state = vm.runInContext('state', context);
  state.csrf = 'test';
  const flush = async () => {for (let i = 0; i < 12; i++) await Promise.resolve();};
  const advance = async ms => {
    const end = now + ms;
    while (true) {
      const next = [...timers].filter(([, t]) => t.due <= end).sort((a,b) => a[1].due-b[1].due)[0];
      if (!next) break;
      const [id, t] = next; now = t.due;
      if (t.interval) t.due += t.ms; else timers.delete(id);
      await t.fn(); await flush();
    }
    now = end;
  };
  return {context, state, $, requests, requestDetails, responses, make, shortcuts, pickers, advance, events,
    setStatus(status, rev = revision) {catalogStatus = status; revision = rev;},
    lastQuery: () => new URLSearchParams(requests.filter(p => p.startsWith('/reader-api/books?')).at(-1).split('?')[1])};
}
const item = kind => ({value: `${kind}-value`, label: kind});

test('upload dialog does not auto-open file picker, preserves batch upload and closes on navigation', async () => {
  const h = harness();
  await h.context.navigate('personal');
  let pickerClicks = 0;
  h.$('uploadFile').click = () => pickerClicks++;
  h.$('openUpload').onclick();
  assert.equal(h.$('uploadDialog').open, true);
  assert.equal(pickerClicks, 0);
  h.$('uploadBookButton').onclick();
  assert.equal(pickerClicks, 1);
  h.$('uploadFile').files = [];
  await h.$('uploadFile').onchange();
  assert.equal(h.$('uploadDialog').open, true);
  h.$('uploadFile').files = [{name:'one.fb2',size:1000},{name:'two.epub',size:1000}];
  h.responses['/reader-api/uploads'] = {id:'uploaded'};
  await h.$('uploadFile').onchange();
  assert.equal(h.$('uploadDialog').open, false);
  assert.equal(h.requests.filter(p => p === '/reader-api/uploads').length, 2);
  assert.equal(h.$('openUpload').disabled, false);
  assert.match(h.$('uploadStatus').textContent, /Обработано: 2 из 2/);
  h.$('openUpload').onclick();
  await h.context.navigate('library');
  assert.equal(h.$('uploadDialog').open, false);
});

test('collections render real cover artwork and reuse the normal catalog without leaking filters', async () => {
  const h = harness();
  h.responses['/reader-api/collections'] = {collections: [{id:'korean-elves', title:'Корейские эльфы',
    image:'/korean-elves.png', description:'Дорамы и волшебство', count:128}], revision:10, status:'ready'};
  await h.context.navigate('collections');
  assert.equal(h.$('collectionsScreen').hidden, false);
  assert.equal(h.$('libraryScreen').hidden, true);
  assert.match(h.$('collectionCards').innerHTML, /href="#collection-korean-elves"/);
  assert.match(h.$('collectionCards').innerHTML, /src="\/korean-elves.png"/);
  assert.match(h.$('collectionCards').innerHTML, /128 книг/);
  await h.context.navigate('collection-korean-elves');
  assert.equal(h.$('collectionsScreen').hidden, true);
  assert.equal(h.$('libraryScreen').hidden, false);
  assert.equal(h.$('collectionBack').hidden, false);
  assert.equal(h.$('viewTitle').textContent, 'Корейские эльфы');
  assert.equal(h.lastQuery().get('collection'), 'korean-elves');
  assert.equal(h.lastQuery().has('shelf'), false);
  await h.context.loadCatalog(true);
  assert.equal(h.lastQuery().get('collection'), 'korean-elves');
  assert.equal(h.lastQuery().get('page'), '2');
  await h.context.navigate('library');
  assert.equal(h.$('collectionBack').hidden, true);
  assert.equal(h.lastQuery().has('collection'), false);
});

test('late collections response cannot replace a different view or account', async () => {
  const h = harness();
  let resolve;
  h.responses['/reader-api/collections'] = new Promise(r => {resolve = r;});
  const pending = h.context.navigate('collections');
  await h.context.navigate('personal');
  resolve({collections:[{id:'private',title:'Stale',count:1}],status:'ready',revision:1});
  await pending;
  assert.equal(h.$('collectionsScreen').hidden, true);
  assert.equal(h.$('collectionCards').innerHTML, undefined);
  assert.equal(h.lastQuery().get('personal'), '1');
});

test('private upload uses raw bytes and CSRF; validation, success and repeat selection', async () => {
  const h = harness();
  await h.context.navigate('personal');
  assert.equal(h.$('uploadControls').hidden, false);
  h.$('uploadFile').files = [{name:'wrong.zip',size:1024}];
  await h.$('uploadFile').onchange();
  assert.equal(h.requests.includes('/reader-api/uploads'), false);
  const file = {name:'My book.FB2', size:3000};
  h.$('uploadFile').files = [file];
  h.responses['/reader-api/uploads'] = {id:'900000000042',title:'Private story'};
  await h.$('uploadFile').onchange();
  const req = h.requestDetails.find(r => r.path === '/reader-api/uploads');
  assert.equal(req.options.method, 'POST');
  assert.equal(req.options.body, file);
  assert.equal(req.options.headers['X-CSRF-Token'], 'test');
  assert.equal(req.options.headers['Content-Type'], 'application/octet-stream');
  assert.equal(req.options.headers['X-Upload-Filename'], encodeURIComponent(file.name));
  assert.equal(req.options.headers['X-Upload-Modified'], '0');
  assert.match(h.$('uploadStatus').textContent, /Обработано: 1 из 1\. Добавлено: 1\./);
  assert.equal(h.$('uploadBookButton').disabled, false);
  assert.equal(h.$('uploadFile').value, '');
  await h.context.navigate('library');
  assert.equal(h.$('uploadControls').hidden, true);
});

test('managed device contents shows pending removal; confirmation sends the owner session command', async () => {
  const h = harness(), remove = h.make();
  remove.dataset.remove = '5';
  h.$('queueList').querySelectorAll = s => s === '[data-remove]' ? [remove] : [];
  h.responses['/reader-api/queue'] = [
    {id:5, book:'42', title:'On device', device:'reader-aabbcc', state:'delivered', action:'download',present:1},
    {id:6, book:'43', title:'To remove', device:'reader-aabbcc', state:'queued', action:'remove',present:1},
  ];
  await h.context.navigate('queue');
  assert.match(h.$('queueList').innerHTML, /Убрать с читалки/);
  assert.match(h.$('queueList').innerHTML, /Удалится при синхронизации/);
  assert.doesNotMatch(h.$('queueList').innerHTML, /Отправить ещё раз/);
  h.context.window.confirm = () => false;
  await remove.onclick();
  assert.equal(h.requests.includes('/reader-api/queue/5/remove'), false);
  h.context.window.confirm = () => true;
  await remove.onclick();
  assert.equal(h.requestDetails.find(r => r.path === '/reader-api/queue/5/remove').options.method, 'POST');
  assert.equal(remove.disabled, false);
});

test('personal collection survives paging and filter pickers, but not unrelated shelves', async () => {
  const h = harness();
  await h.context.navigate('personal', {personalCollection:'shelf-1'});
  assert.equal(h.lastQuery().get('personalCollection'), 'shelf-1');
  await h.context.loadCatalog(true);
  assert.equal(h.lastQuery().get('personalCollection'), 'shelf-1');
  assert.equal(h.lastQuery().get('page'), '2');
  const origin = h.context.catalogContext();
  await h.context.chooseFacet('author', item('author'), origin, true);
  assert.equal(h.lastQuery().get('personalCollection'), 'shelf-1');
  await h.context.navigate('library');
  assert.equal(h.lastQuery().has('personalCollection'), false);
});

test('real upload handler sends all selected files once and assigns the selected collection', async () => {
  const h = harness();
  await h.context.navigate('personal', {personalCollection:'shelf-1'});
  h.responses['/reader-api/uploads'] = {id:'900000000042',title:'FB2 title'};
  h.$('uploadFile').files = [{name:'a.fb2',size:3000}, {name:'bad.zip',size:3000}, {name:'b.fb2',size:3000}];
  await h.$('uploadFile').onchange();
  const uploads = h.requestDetails.filter(r => r.path === '/reader-api/uploads');
  assert.equal(uploads.length, 2);
  const membership = h.requestDetails.filter(r => r.path.endsWith('/900000000042/personal-collections'));
  assert.equal(membership.length, 2);
  assert.equal(JSON.parse(membership[0].options.body).id, 'shelf-1');
  assert.match(h.$('uploadStatus').textContent, /Обработано: 3 из 3\. Добавлено: 2\./);
  assert.equal(h.$('uploadErrors').hidden, false);
  assert.match(h.$('uploadErrorsList').children[0].textContent, /bad.zip/);
});

test('independent destinations replace all previous filters (including group/tag/series)', async () => {
  for (const [from, to, directory] of [
    ['author','genre','genres'], ['genre','author','authors'], ['series','tag','tags'],
    ['tag','series','series'], ['genreGroup','genre','genres'], ['tagGroup','author','authors'],
  ]) {
    const h = harness();
    await h.context.chooseFacet(from, item(from));
    await h.context.navigate(directory);
    await h.context.chooseFacet(to, item(to));
    const q = h.lastQuery();
    assert.equal(q.get(from), null, `${from} leaked into ${to}`);
    assert.equal(q.get(to), item(to).value);
    assert.equal(q.get('page'), '1');
  }
});

test('All books and unrelated shelves clear search, scope, filters and pagination', async () => {
  for (const view of ['library','recent','personal','reading','favorite','want','read']) {
    const h = harness();
    h.state.filters = {series: item('series'), author: item('author')};
    h.$('searchInput').value = 'old query'; h.$('searchScope').value = 'title';
    h.state.page = 5;
    await h.context.navigate(view);
    const q = h.lastQuery();
    assert.equal(q.get('q'), null); assert.equal(q.get('scope'), 'all');
    assert.equal(q.get('author'), null); assert.equal(q.get('series'), null);
    assert.equal(q.get('page'), '1');
  }
});

test('shortcuts are navigation, filter panel intentionally combines, Back restores its origin', async () => {
  const h = harness();
  await h.context.chooseFacet('author', item('author'));
  await h.shortcuts[0].onclick();
  await h.context.chooseFacet('genre', item('genre'));
  assert.equal(h.lastQuery().get('author'), null);
  await h.pickers[2].onclick();
  await h.context.chooseFacet('author', item('author'));
  assert.equal(h.lastQuery().get('genre'), item('genre').value);
  assert.equal(h.lastQuery().get('author'), item('author').value);
  h.$('searchInput').value = 'intentional search';
  await h.pickers[3].onclick();
  await h.$('backToBooks').onclick();
  assert.equal(h.lastQuery().get('q'), 'intentional search');
  assert.equal(h.lastQuery().get('author'), item('author').value);
});

test('search to category starts fresh; same-context pagination preserves intentional filters', async () => {
  const h = harness();
  h.$('searchInput').value = 'old query';
  h.state.filters = {author: item('author')};
  await h.context.navigate('genres');
  await h.context.chooseFacet('genreGroup', item('genreGroup'));
  assert.equal(h.lastQuery().get('q'), null);
  assert.equal(h.lastQuery().get('author'), null);
  h.$('searchInput').value = 'within genre';
  await h.context.loadCatalog(); await h.context.loadCatalog(true);
  assert.equal(h.lastQuery().get('page'), '2');
  assert.equal(h.lastQuery().get('q'), 'within genre');
  assert.equal(h.lastQuery().get('genreGroup'), item('genreGroup').value);
});

test('indexing never replaces cards, focus or scroll during 65 seconds, including load-more', async () => {
  const h = harness(); h.setStatus('loading');
  await h.context.enter({username: 'test', csrf: 'test', devices: []});
  await h.context.loadCatalog(true);
  const grid = h.$('bookGrid'), first = grid.children[0], replacements = grid.replacements;
  h.context.window.scrollY = 750; h.context.document.activeElement = first;
  const before = h.requests.filter(p => p.startsWith('/reader-api/books?')).length;
  await h.advance(65000);
  assert.equal(h.requests.filter(p => p.startsWith('/reader-api/books?')).length, before);
  assert.equal(grid.replacements, replacements); assert.equal(grid.children[0], first);
  assert.equal(h.state.page, 2); assert.equal(h.context.window.scrollY, 750);
  assert.equal(h.context.document.activeElement, first);
  h.setStatus('ready', 11); await h.context.checkCatalogUpdates();
  assert.equal(h.$('catalogUpdates').hidden, false);
  assert.equal(grid.children[0], first);
  await h.$('catalogUpdates').onclick();
  assert.equal(h.state.page, 1); assert.equal(h.$('catalogUpdates').hidden, true);
});

test('polling and pending search cannot overwrite a directory or hidden app', async () => {
  const h = harness(); h.setStatus('loading');
  await h.context.enter({username: 'test', csrf: 'test', devices: []});
  h.$('searchInput').value = 'pending'; h.$('searchInput').oninput();
  await h.context.navigate('authors');
  const before = h.requests.filter(p => p.startsWith('/reader-api/books?')).length;
  await h.advance(65000);
  assert.equal(h.state.view, 'authors');
  assert.equal(h.requests.filter(p => p.startsWith('/reader-api/books?')).length, before);
  h.context.document.hidden = true;
  const count = h.requests.length; await h.advance(65000);
  assert.equal(h.requests.length, count);
});

test('book metadata links always leave any combined/personal context behind', async () => {
  const h = harness();
  await h.context.navigate('personal');
  h.state.filters = {author: item('author'), genre: item('genre')};
  await h.pickers[3].onclick();
  await h.context.chooseFacet('series', item('series'), null, false);
  const q = h.lastQuery();
  assert.equal(h.state.view, 'library');
  assert.equal(q.get('personal'), null); assert.equal(q.get('author'), null);
  assert.equal(q.get('genre'), null); assert.equal(q.get('series'), item('series').value);
  assert.equal(q.get('sort'), 'series');
});

test('slow polling response cannot overwrite a newly navigated page or new login', async () => {
  for (const change of ['route', 'account']) {
    const h = harness();
    await h.context.navigate('library');
    const fetch = h.context.fetch;
    let finish;
    h.context.fetch = path => path === '/reader-api/catalog-status'
      ? new Promise(resolve => {finish = () => resolve({ok: true,
          json: async () => ({status: 'loading', revision: 11})});}) : fetch(path);
    const pending = h.context.checkCatalogUpdates();
    if (change === 'route') await h.context.navigate('favorite');
    else h.state.csrf = 'new-login';
    h.$('catalogStatus').textContent = 'current status';
    finish(); await pending;
    assert.equal(h.$('catalogStatus').textContent, 'current status');
  }
});

test('ready author/series/genre pages remain idle for 65 seconds', async () => {
  for (const kind of ['author', 'series', 'genre']) {
    const h = harness();
    await h.context.enter({username: 'test', csrf: 'test', devices: []});
    await h.context.chooseFacet(kind, item(kind));
    const first = h.$('bookGrid').children[0], url = h.context.location.hash;
    const before = h.requests.filter(p => p.startsWith('/reader-api/books?')).length;
    await h.advance(65000);
    assert.equal(h.requests.filter(p => p.startsWith('/reader-api/books?')).length, before);
    assert.equal(h.$('bookGrid').children[0], first); assert.equal(h.context.location.hash, url);
    assert.equal(h.lastQuery().get(kind), item(kind).value);
  }
});
