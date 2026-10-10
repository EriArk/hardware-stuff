const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const {uploadBatch} = require('../src/octofox_library/web/personal-library.js');
const file = name => ({name, size: 1000});

test('batch uploads every FB2 sequentially, continues after errors, and reports progress', async () => {
  const calls = [], reports = []; let active = 0;
  const result = await uploadBatch([file('one.fb2'), file('bad.zip'), file('bad.fb2'), file('two.FB2')], {
    upload: async f => {assert.equal(++active, 1); calls.push(f.name); await Promise.resolve(); active--; if (f.name === 'bad.fb2') throw Error('Broken XML');},
    report: p => reports.push(p),
  });
  assert.deepEqual(calls, ['one.fb2', 'bad.fb2', 'two.FB2']);
  assert.equal(result.added, 2); assert.equal(result.done, 4); assert.equal(result.errors.length, 2);
  assert.equal(reports.at(-1).done, 4);
});
test('429 pauses and retries same file; cancellation and session loss never start another request', async () => {
  let calls = 0, waits = 0, alive = true;
  const result = await uploadBatch([file('one.fb2')], {upload: async () => {
    if (++calls === 1) throw Object.assign(Error('busy'), {status:429, retryAfter:2});
  }, sleep: async () => waits++});
  assert.equal(result.added, 1); assert.equal(calls, 2); assert.equal(waits, 2);
  calls = 0;
  const cancelled = await uploadBatch([file('a.fb2'),file('b.fb2')], {alive: () => alive,
    upload: async () => {calls++; alive = false;}});
  assert.equal(calls, 1); assert.equal(cancelled.stopped, true);
  const expired = await uploadBatch([file('a.fb2'), file('b.fb2')], {upload: async () => {throw Object.assign(Error('login'), {status:401});}});
  assert.equal(expired.done, 1); assert.equal(expired.stopped, true);
});
test('size validation and bounded retry leave useful per-file errors', async () => {
  let calls = 0;
  const result = await uploadBatch([{name:'empty.fb2',size:0}, {name:'huge.fb2',size:17*1024*1024}, file('a.fb2')], {
    upload: async () => {calls++; throw Object.assign(Error('busy'), {status:429,retryAfter:1});}, sleep: async () => {},
  });
  assert.equal(calls, 4); assert.equal(result.errors.length, 3);
});
test('batch picker, compact collection controls and external module ship with cache busting', () => {
  const html = fs.readFileSync('src/octofox_library/web/index.html', 'utf8');
  assert.match(html, /id="uploadFile"[^>]*multiple/);
  const personalAsset = html.match(/\/personal-library\.js\?v=(\d+)/);
  const appAsset = html.match(/\/app\.js\?v=(\d+)/);
  assert(personalAsset && Number(personalAsset[1]) >= 3);
  assert(appAsset && Number(appAsset[1]) >= 42);
  assert(personalAsset.index < appAsset.index);
  for (const id of ['personalCollection','personalCollectionsDialog','personalCollectionForm','uploadErrors']) assert(html.includes(`id="${id}"`));
});

test('upload lives in compact toolbar; help and picker live in a labelled dialog', () => {
  const html = fs.readFileSync('src/octofox_library/web/index.html', 'utf8');
  const css = fs.readFileSync('src/octofox_library/web/styles.css', 'utf8');
  const toolbar = html.slice(html.indexOf('<div class="personal-collection-toolbar"'), html.indexOf('<input id="uploadFile"'));
  assert.match(toolbar, /id="openUpload"/);
  assert.match(toolbar, /aria-label="Загрузить файлы"/);
  assert.doesNotMatch(toolbar, /id="uploadBookButton"/);
  const dialog = html.slice(html.indexOf('<dialog id="uploadDialog"'), html.indexOf('<dialog id="personalCollectionsDialog"'));
  assert.match(dialog, /id="uploadBookButton"/);
  assert.match(dialog, /aria-labelledby="uploadHeading"/);
  assert.match(dialog, /до 16 МБ/);
  assert.match(css, /grid-template-columns:minmax\(0, 1fr\) auto 44px/);
  assert.match(css, /#uploadStatus:empty \{ display:none; \}/);
});

test('mixed EPUB and FB2 batches accept books but not arbitrary archives', async () => {
  const calls = [];
  const result = await uploadBatch([file('a.epub'), file('b.EPUB'), file('c.fb2'), file('d.zip')], {
    upload: async f => calls.push(f.name),
  });
  assert.deepEqual(calls, ['a.epub','b.EPUB','c.fb2']);
  assert.equal(result.added, 3); assert.equal(result.errors.length, 1);
});

test('external source navigation is absent from the library', () => {
  const html = fs.readFileSync('src/octofox_library/web/index.html', 'utf8');
  const app = fs.readFileSync('src/octofox_library/web/app.js', 'utf8');
  assert(!html.includes('catalog-sources')); assert(!app.includes('catalogSources'));
  assert.doesNotMatch(html, /discoveryNav|flibusta/i);
});

function controllerHarness() {
  const vm = require('node:vm'), nodes = new Map(), calls = [], filters = [];
  const make = tag => ({tag, children: [], textContent: '', value: '', open: false, isConnected: true,
    attrs: {}, setAttribute(k,v) {this.attrs[k] = v;}, focus() {},
    append(...children) {this.children.push(...children);}, replaceChildren(...children) {this.children = children;},
    querySelectorAll(tag) {return this.children.flatMap(c => [...(c.tag === tag ? [c] : []), ...c.querySelectorAll(tag)]);},
    showModal() {this.open = true;}, close() {this.open = false;}});
  const $ = id => {if (!nodes.has(id)) nodes.set(id,make('div'));return nodes.get(id);};
  let account = 'alice', items = [{id:'one',name:'<b>Сказки</b>',count:1}], members = new Set();
  const api = async (path, data) => {
    calls.push([path,data]);
    if (data?.action === 'create') items.push({id:'two',name:data.name,count:0});
    if (data?.action === 'rename') items.find(c=>c.id===data.id).name = data.name;
    if (data?.action === 'delete') items = items.filter(c=>c.id!==data.id);
    if (path.includes('/books/')) {
      if (data) data.selected ? members.add(data.id) : members.delete(data.id);
      return {inLibrary:true, collections:items.map(c=>({...c,selected:members.has(c.id)}))};
    }
    return {collections:items.map(c=>({...c})),id:'two'};
  };
  const ctx = {document:{getElementById:$,createElement:make}, window:{confirm:()=>true,prompt:()=> 'Renamed'}};
  vm.createContext(ctx);
  vm.runInContext(fs.readFileSync('src/octofox_library/web/personal-library.js','utf8'),ctx);
  const controller = ctx.window.BookPersonal.createPersonalLibrary({api:(...args)=>ctx.api(...args),session:()=>account,
    onFilter:v=>filters.push(v),onChange:()=>calls.push(['changed'])});
  ctx.api = api;
  return {controller,$,make,ctx,calls,filters,account:v=>account=v,
    flush:async()=>{for(let i=0;i<30;i++) await Promise.resolve();}};
}
test('personal collections control creates, renames, deletes, selects and escapes names', async () => {
  const h = controllerHarness(); await h.controller.enter('one');
  assert.equal(h.$('personalCollection').value, 'one');
  assert.equal(h.$('personalCollection').children[1].textContent,'<b>Сказки</b> · 1');
  assert.equal(h.$('personalCollection').children[1].innerHTML,undefined);
  h.$('managePersonalCollections').onclick(); await h.flush();
  assert.equal(h.$('personalCollectionsDialog').open,true);
  h.$('personalCollectionName').value = 'New';
  h.$('personalCollectionForm').onsubmit({preventDefault(){}}); await h.flush();
  assert.equal(h.$('personalCollectionsList').children.length,2);
  h.$('personalCollectionsList').children[0].children[1].onclick(); await h.flush();
  assert.match(h.$('personalCollectionsList').children[0].children[0].textContent,/Renamed/);
  h.$('personalCollectionsList').children[0].children[2].onclick(); await h.flush();
  assert.equal(h.$('personalCollection').value,''); assert.equal(h.filters.at(-1),'');
  h.$('personalCollectionsList').children[0].children[0].onclick();
  assert.equal(h.filters.at(-1),'two'); assert.equal(h.$('personalCollectionsDialog').open,false);
});
test('book card supports independent memberships and create-and-add without changing reading progress', async () => {
  const h = controllerHarness(), host = h.make('div'), book = {id:'42',inLibrary:false};
  let changes = 0;
  h.controller.attachBook(host,book,()=>changes++);
  const details = host.children[0]; details.open = true; details.ontoggle(); await h.flush();
  const checkbox = details.children[1].children[0].children[0];
  checkbox.checked = true; checkbox.onchange(); await h.flush();
  assert.equal(book.inLibrary,true); assert.equal(changes,1);
  const form = details.children[2]; form.children[0].value = 'New';
  await form.onsubmit({preventDefault(){}}); await h.flush();
  assert.equal(changes,2);
  assert(h.calls.some(([p,d])=>p==='/books/42/personal-collections' && d?.id==='two' && d.selected));
  assert(!h.calls.some(([p])=>p.endsWith('/state') || p.includes('queue')));
  assert.match(details.children[0].textContent,/2/);
});
test('logout ignores in-flight collection results and clears private UI', async () => {
  const h = controllerHarness(); let resolve;
  h.ctx.api = () => new Promise(r=>resolve=r);
  const pending = h.controller.enter('one');
  h.account('bob'); h.controller.reset();
  resolve({collections:[{id:'one',name:'Private',count:10}]}); await pending;
  assert.equal(h.$('personalCollection').children.length,1);
  assert.equal(h.$('personalCollection').value,'');
});
