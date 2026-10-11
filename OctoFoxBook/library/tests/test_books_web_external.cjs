const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('src/octofox_library/web/external-catalogs.js','utf8');

function harness() {
  const elements = new Map(), nodes = [], calls = [], opened = [];
  function make() {
    const e = {dataset:{},children:[],hidden:false,disabled:false,textContent:'',attrs:{},
      append(...items) {this.children.push(...items);}, replaceChildren() {this.children=[];},
      setAttribute(k,v) {this.attrs[k]=v;}, querySelector() {return null;},
      reset() {}, close() {}, showModal() {}, focus() {},
      remove() {for(const n of nodes) n.children=n.children.filter(x=>x!==this);},
    }; nodes.push(e); return e;
  }
  const $ = id => {if(!elements.has(id)) elements.set(id,make());return elements.get(id);};
  $('downloadsScreen').hidden = true;
  const state = {catalogs:[{id:'one',name:'One',url:'https://one.example/'}],jobs:[],browse:async()=>({items:[],next:''})};
  const api = async (url,data) => {
    calls.push({url,data});
    if(url==='/external/catalogs')return {catalogs:state.catalogs};
    if(url==='/external/downloads')return data ? {id:'job'} : {downloads:state.jobs};
    if(url.startsWith('/external/browse?'))return state.browse(new URLSearchParams(url.split('?')[1]));
    return {};
  };
  const ctx={window:{},document:{getElementById:$,createElement:make,
    querySelectorAll:()=>nodes.filter(n=>n.dataset.externalItem)},URLSearchParams,URL,
    setInterval(){},clearInterval(){},confirm:()=>true};
  vm.runInNewContext(source,ctx);
  const ui=ctx.window.ExternalCatalogs({api,session:()=>true,onChange(){},onSourcesChanged(){},
    openBook:id=>opened.push(id),toast(){},cover:()=>'<cover/>',bindCovers(){},esc:s=>String(s||'').replaceAll('<','&lt;')});
  return {ui,$,state,calls,opened};
}
const book=(id='a',extra={})=>({id,title:'Book '+id,author:'Author',available:true,cover:false,formats:[],...extra});

test('all sources share one grid and identical provider IDs stay independently selectable',async()=>{
  const h=harness();h.state.catalogs.push({id:'two',name:'Two',url:'https://two.example/'});
  h.state.browse=async()=>({items:[book()],next:''});
  await h.ui.shelf(false,h.$('bookGrid'));
  const tiles=h.$('bookGrid').children;assert.equal(tiles.length,2);
  assert.notEqual(tiles[0].dataset.publication,tiles[1].dataset.publication);
  await tiles[1].children[1].onclick();
  assert.equal(h.calls.find(c=>c.url==='/external/downloads'&&c.data).data.catalog,'two');
});

test('import identity from feed suppresses duplicate after download history is cleared',async()=>{
  const h=harness(),local={dataset:{localBook:'42'}};h.$('bookGrid').append(local);
  h.state.browse=async()=>({items:[book('a',{book:'42'})],next:''});
  await h.ui.shelf(false,h.$('bookGrid'));
  assert.equal(h.$('bookGrid').children.length,1);
  h.$('bookGrid').replaceChildren();await h.ui.shelf(false,h.$('bookGrid'));
  const tile=h.$('bookGrid').children[0];assert.equal(tile.dataset.localBook,'42');
  await tile.children[1].onclick();assert.deepEqual(h.opened,['42']);
  assert.equal(h.calls.filter(c=>c.url==='/external/downloads'&&c.data).length,0);
});

test('load more follows each feed next URL without repeating query or duplicate entries',async()=>{
  const h=harness();h.state.browse=async q=>q.get('url') ? {items:[book(),book('b')],next:''} : {items:[book()],next:'https://one.example/page2'};
  assert.equal(await h.ui.shelf(false,h.$('bookGrid'),{query:'story'}),true);
  assert.equal(await h.ui.shelf(true,h.$('bookGrid'),{query:'story'}),false);
  assert.equal(h.$('bookGrid').children.length,2);
  const q=new URLSearchParams(h.calls.filter(c=>c.url.startsWith('/external/browse?'))[1].url.split('?')[1]);
  assert.equal(q.get('url'),'https://one.example/page2');assert.equal(q.get('q'),'');
});

test('late remote response cannot repaint another view or logged-out session',async()=>{
  for(const action of ['invalidate','reset']) {
    const h=harness();let resolve,started;
    const ready=new Promise(r=>started=r);
    h.state.browse=()=>new Promise(r=>{resolve=r;started();});
    const pending=h.ui.shelf(false,h.$('bookGrid'));
    await ready;h.ui[action]();
    resolve({items:[book()],next:''});await pending;
    assert.equal(h.$('bookGrid').children.length,0);
  }
});

test('failed provider does not hide another provider or local books',async()=>{
  const h=harness();h.state.catalogs.push({id:'two',name:'Two',url:'https://two.example/'});
  h.state.browse=async q=>{if(q.get('catalog')==='one')throw Error('Offline');return {items:[book()],next:''};};
  h.$('bookGrid').append({dataset:{localBook:'42'}});
  assert.equal(await h.ui.shelf(false,h.$('bookGrid')),true);
  assert.equal(h.$('bookGrid').children.length,2);
  assert.match(h.$('externalShelfStatus').textContent,/One: Offline/);
});

test('completion deduplicates a second source importing the same book',async()=>{
  const h=harness();h.state.browse=async()=>({items:[book()],next:''});
  await h.ui.shelf(false,h.$('bookGrid'));
  h.$('bookGrid').append({dataset:{localBook:'42'}});
  h.state.jobs=[{id:'j',catalog:'one',item:'a',title:'Book',state:'complete',book:'42'}];
  await h.ui.enter();assert.equal(h.$('bookGrid').children.length,1);
});

test('author filters and escaped shared captions apply to remote tiles',async()=>{
  const h=harness();h.state.browse=async()=>({items:[book('a',{author:'Other'}),book('b',{title:'<img onerror=bad>',author:'Jane'})],next:''});
  await h.ui.shelf(false,h.$('bookGrid'),{filters:{author:{label:'Jane'}}});
  assert.equal(h.$('bookGrid').children.length,1);
  assert.match(h.$('bookGrid').children[0].innerHTML,/&lt;img/);
});
