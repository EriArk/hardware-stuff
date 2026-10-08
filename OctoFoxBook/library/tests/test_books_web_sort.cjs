const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('src/octofox_library/web/app.js', 'utf8');
function harness() {
  const inputs = ['recent','title','title_desc','author','series'].map(value => ({value, focus() {this.focused = true;}}));
  const dialog = {open:false, style:{}, offsetWidth:290, offsetHeight:320,
    show() {this.open = true;}, close() {this.open = false;}, contains(t) {return inputs.includes(t);},
    querySelectorAll: () => inputs, querySelector: () => inputs.find(i => i.checked)};
  const button = {setAttribute(k,v) {this[k]=v;}, focus() {this.focused=true;}, contains: () => false,
    getBoundingClientRect: () => ({right:378,bottom:250})};
  const nodes = {personalSortDialog:dialog, personalSortButton:button, sortFilter:{value:'recent'}, closePersonalSort:{}};
  const events = {}, ctx = {state:{view:'personal'}, $:id=>nodes[id], task:fn=>fn, calls:0,
    document:{addEventListener(k,fn) {events[k]=fn;}},
    window:{innerWidth:390,innerHeight:844,addEventListener(k,fn) {events[k]=fn;}}};
  ctx.loadCatalog = async () => {ctx.calls++;};
  vm.createContext(ctx);
  vm.runInContext(source.slice(source.indexOf('function closePersonalSort('),source.indexOf('for (const id of ["searchScope", "sortFilter"])')),ctx);
  return {ctx,nodes,dialog,button,inputs,events};
}
test('sort opens anchored non-modal list with selected value and toggles closed', () => {
  const h=harness(); h.nodes.sortFilter.value='author'; h.button.onclick();
  assert(h.dialog.open); assert.equal(h.button['aria-expanded'],'true');
  assert(h.inputs[3].checked && h.inputs[3].focused);
  assert.equal(h.dialog.style.left,'88px'); h.button.onclick(); assert(!h.dialog.open);
});
test('selection uses server catalog order and closes; no reload for unchanged or invalid choice', async () => {
  const h=harness(); h.button.onclick();
  await h.dialog.onchange({target:h.inputs[1]});
  assert.equal(h.nodes.sortFilter.value,'title'); assert.equal(h.ctx.calls,1);
  assert(!h.dialog.open); assert(h.button.focused);
  await h.dialog.onchange({target:h.inputs[1]});
  await h.dialog.onchange({target:{value:'unknown'}}); assert.equal(h.ctx.calls,1);
  h.ctx.state.view='library'; await h.dialog.onchange({target:h.inputs[0]}); assert.equal(h.ctx.calls,1);
});
test('outside tap, Escape, scroll, and navigation dismiss without changing order', () => {
  const h=harness();
  for (const name of ['pointerdown','focusin','scroll','keydown']) {
    h.button.onclick(); h.events[name]({target:{},key:'Escape',preventDefault(){}}); assert(!h.dialog.open);
  }
  assert.equal(h.ctx.calls,0);
  assert.match(source,/async function navigate\(view, context = \{\}\) \{\s*closePersonalSort\(\)/);
  assert(source.includes('sort: $("sortFilter").value'));
  assert(source.includes('query.set("personal", "1")'));
});
