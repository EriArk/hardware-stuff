// Real web assets, isolated account fixture; never mutates a live library.
const fs = require('node:fs'), path = require('node:path'), http = require('node:http');
const assert = require('node:assert/strict');
const {readAsset, english} = require('./localized-assets.cjs');
const {chromium} = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const root = path.resolve('src/octofox_library/web');
const output = path.resolve(process.env.QA_OUTPUT || 'test-results/profile-sync');
fs.mkdirSync(output, {recursive:true});
let enabled = true, reject = false;
const writes = [], queries = [];
const book = {id:'42', title:'Sea of Quiet', author:'Elena North', reading:{shelf:'favorite'}, inLibrary:true};
const server = http.createServer(async (req,res) => {
  const url = new URL(req.url, 'http://localhost');
  if (url.pathname.startsWith('/reader-api/')) {
    let data = {};
    if (url.pathname.endsWith('/profile-sync')) {
      assert.equal(req.headers['x-csrf-token'], 'fixture');
      let body = ''; for await (const chunk of req) body += chunk;
      const value = JSON.parse(body); writes.push(value);
      if (reject) { res.writeHead(409, {'Content-Type':'application/json'}); res.end(JSON.stringify({error:'Try again'})); return; }
      enabled = value.enabled; data = {profile_sync:enabled};
    } else if (url.pathname.endsWith('/me')) data = {csrf:'fixture', username:'Reader', devices:[], flibustaAdmin:false};
    else if (url.pathname.endsWith('/devices')) data = [{id:'reader-fixture',name:'AbyssBook',last_seen:1,profile_sync:enabled}];
    else if (url.pathname.endsWith('/queue')) data = [{id:1,book:'42',title:book.title,device:'reader-fixture',state:'delivered',action:'download'}];
    else if (url.pathname.endsWith('/catalog-status')) data = {status:'ready',revision:1};
    else if (url.pathname.endsWith('/personal-collections')) data = {collections:[{id:'travel',name:'Journeys',count:1}]};
    else if (url.pathname.endsWith('/books')) { queries.push(url.search); data = {books:[book],total:1,more:false,indexStatus:'ready',catalogRevision:1}; }
    res.writeHead(200, {'Content-Type':'application/json'}); res.end(JSON.stringify(data)); return;
  }
  const file = path.resolve(root, '.'+(url.pathname==='/' ? '/index.html' : url.pathname));
  if (!file.startsWith(root+path.sep) || !fs.existsSync(file) || !fs.statSync(file).isFile()) { res.writeHead(404); res.end(); return; }
  res.setHeader('Content-Type',file.endsWith('.js') ? 'text/javascript' : file.endsWith('.css') ? 'text/css' : file.endsWith('.html') ? 'text/html' : 'application/octet-stream');
  res.end(readAsset(file));
});
(async () => {
  await new Promise(resolve => server.listen(0,'127.0.0.1',resolve));
  const browser = await chromium.launch({headless:true, executablePath:process.env.CHROMIUM_EXECUTABLE});
  try {
    const page = await browser.newPage({viewport:{width:1280,height:900}});
    const errors = []; page.on('pageerror',e => errors.push(e.message));
    const base = `http://127.0.0.1:${server.address().port}/`;
    await page.goto(base+'#queue');
    const checkbox = page.locator('[data-profile-sync]');
    await checkbox.waitFor(); assert(await checkbox.isChecked());
    assert(await page.locator('[data-remove]').isHidden());
    if (english) assert((await page.locator('#deviceList').innerText()).includes('Sync my whole profile'));
    await page.screenshot({path:path.join(output,'profile-desktop.png')});
    await checkbox.uncheck(); await page.waitForFunction(() => !document.querySelector('[data-profile-sync]').disabled);
    assert.equal(enabled,false); assert(await page.locator('[data-remove]').isVisible());
    await page.reload(); await checkbox.waitFor(); assert(!(await checkbox.isChecked()));
    reject = true; await checkbox.click(); await page.waitForFunction(() => !document.querySelector('[data-profile-sync]').disabled);
    assert(!(await checkbox.isChecked())); assert.equal(enabled,false);
    reject = false; await checkbox.check(); await page.waitForFunction(() => !document.querySelector('[data-profile-sync]').disabled);
    assert.equal(enabled,true); assert.deepEqual(writes,[{enabled:false},{enabled:true},{enabled:true}]);
    await page.reload(); await checkbox.waitFor();
    await page.setViewportSize({width:393,height:852});
    await page.waitForFunction(() => document.querySelector('#drawer').inert && document.querySelector('#drawer').getBoundingClientRect().right <= 0);
    await page.screenshot({path:path.join(output,'profile-mobile.png')});
    assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
    await page.goto(base+'#favorite');
    const select = page.locator('#personalCollection'); await select.waitFor();
    await page.waitForFunction(() => document.querySelector('#personalCollection').options.length===2);
    assert((await select.locator('option').first().innerText()) === (english ? 'Favorites' : 'Избранное'));
    assert(await page.locator('#openUpload').isHidden());
    await select.selectOption('travel');
    await page.waitForFunction(() => state.personalCollection==='travel' && !state.busy);
    assert(queries.some(q => q.includes('personalCollection=travel') && !q.includes('shelf=favorite')));
    await page.screenshot({path:path.join(output,'favorites-collection-mobile.png')});
    await select.selectOption('');
    await page.waitForFunction(() => state.personalCollection==='' && !state.busy);
    assert(queries.at(-1).includes('shelf=favorite'));
    assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
    assert.deepEqual(errors,[]);
    console.log('PROFILE_BROWSER_OK', english ? 'en' : 'ru');
  } finally { await browser.close(); server.close(); }
})().catch(error => {console.error(error); server.close(); process.exitCode=1;});
