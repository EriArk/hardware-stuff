// Local UI contract fixture. No real installation or accounts are changed.
const fs = require('node:fs'), path = require('node:path'), http = require('node:http');
const assert = require('node:assert/strict');
const {chromium, webkit} = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const root = path.resolve('src/octofox_library/web');
const output = path.resolve(process.env.QA_OUTPUT || 'test-results/companion');
fs.mkdirSync(output, {recursive:true});
let configured = false, loggedIn = false;
const accounts = [{id:1,username:'owner',name:'Александра',admin:true}];
const server = http.createServer(async (req,res) => {
  const url = new URL(req.url, 'http://localhost');
  if (url.pathname.startsWith('/companion-api/')) {
    let bytes = ''; for await (const part of req) bytes += part;
    const form = JSON.parse(bytes || '{}');
    const route = url.pathname.slice('/companion-api/'.length);
    let data = {}, status = 200;
    if (route === 'status') data = {configured};
    else if (route === 'setup' || route === 'login') {
      if (route === 'setup') assert.equal(req.headers['x-setup-key'], 'fixture-key');
      assert.equal(form.password, 'fixture-password');
      configured = loggedIn = true; data = {csrf:'fixture',user:accounts[0],readerReady:true};
    } else if (!loggedIn) { status = 401; data = {error:'Войдите в Companion.'}; }
    else if (route === 'me') data = {csrf:'fixture',user:accounts[0]};
    else if (route === 'users' && req.method === 'GET') data = {users:accounts};
    else if (route === 'users') {
      accounts.push({id:accounts.length+1,username:form.username,name:form.name,admin:false});
      status = 201; data = {readerReady:true};
    } else if (route === 'reader-access') data = {readerReady:true};
    else if (route === 'logout') loggedIn = false;
    else status = 404;
    res.writeHead(status, {'Content-Type':'application/json'}); return res.end(JSON.stringify(data));
  }
  const relative = url.pathname === '/companion' ? 'companion.html' : url.pathname.slice(1);
  const filename = path.resolve(root, relative);
  if (!filename.startsWith(root + path.sep) || !fs.existsSync(filename)) {res.writeHead(404); return res.end();}
  const mime = {'.html':'text/html; charset=utf-8','.js':'text/javascript; charset=utf-8','.css':'text/css','.woff2':'font/woff2','.svg':'image/svg+xml'}[path.extname(filename)];
  res.writeHead(200, {'Content-Type':mime || 'application/octet-stream'}); res.end(fs.readFileSync(filename));
});

(async () => {
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const browser = await (process.env.TEST_WEBKIT ? webkit : chromium).launch({headless:true});
  try {
    const page = await browser.newPage({viewport:{width:1280,height:980}});
    const errors = []; page.on('pageerror', e => errors.push(e.message));
    await page.goto(`http://127.0.0.1:${server.address().port}/companion`);
    await page.locator('#welcome').waitFor({state:'visible'});
    await page.screenshot({path:path.join(output,'setup-desktop.png'),fullPage:true});
    for (const width of [393,768,1024,1280]) {
      await page.setViewportSize({width,height:980});
      assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), `overflow at ${width}`);
    }
    await page.setViewportSize({width:393,height:852});
    await page.screenshot({path:path.join(output,'setup-mobile.png'),fullPage:true});
    const form = page.locator('#identity-form');
    for (const [name,value] of Object.entries({setupKey:'fixture-key',name:'Александра',email:'owner@example.org',username:'owner',password:'fixture-password'})) await form.locator(`[name="${name}"]`).fill(value);
    await form.locator('button').click();
    await page.locator('#users li').waitFor();
    await page.setViewportSize({width:1280,height:900});
    await page.locator('#add-reader').click();
    await page.screenshot({path:path.join(output,'new-reader-desktop.png'),fullPage:true});
    const newForm = page.locator('#reader-form');
    for (const [name,value] of Object.entries({name:'<script>window.injected = true</script>',email:'reader@example.org',username:'reader',password:'new-fixture-password'})) await newForm.locator(`[name="${name}"]`).fill(value);
    await newForm.locator('button').click();
    await page.locator('#users li').nth(1).waitFor();
    assert.equal(await page.evaluate(() => window.injected), undefined);
    assert.equal(await page.locator('#users li').nth(1).locator('strong').textContent(), '<script>window.injected = true</script>');
    accounts[1].name = 'Михаил'; await page.locator('#refresh-users').click();
    await page.getByText('Михаил', {exact:true}).waitFor();
    await page.screenshot({path:path.join(output,'readers-desktop.png'),fullPage:true});
    await page.setViewportSize({width:393,height:852});
    assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
    await page.screenshot({path:path.join(output,'readers-mobile.png'),fullPage:true});
    await page.locator('#logout').click();
    await page.getByRole('heading',{name:'Войти в Companion'}).waitFor();
    assert.equal(await page.locator('#setup-fields').isVisible(), false);
    await form.locator('[name="username"]').fill('owner');
    await form.locator('[name="password"]').fill('fixture-password');
    await form.locator('button').click();
    await page.locator('#dashboard').waitFor({state:'visible'});
    await page.reload(); await page.locator('#dashboard').waitFor({state:'visible'});
    assert.deepEqual(errors, []);
    console.log('Companion UI passed: setup, create reader, safe text, logout/login, reload; widths 393–1280.');
  } finally {await browser.close(); await new Promise(resolve => server.close(resolve));}
})().catch(error => {console.error(error); process.exitCode = 1; server.close();});
