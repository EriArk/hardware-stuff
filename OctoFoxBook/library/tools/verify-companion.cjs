// Local UI contract fixture. No real installation or accounts are changed.
const fs = require('node:fs'), path = require('node:path'), http = require('node:http');
const assert = require('node:assert/strict');
const {execFileSync} = require('node:child_process');
const {english, readAsset} = require('./localized-assets.cjs');
const {chromium, webkit} = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const root = path.resolve('src/octofox_library/web');
const output = path.resolve(process.env.QA_OUTPUT || 'test-results/companion');
fs.mkdirSync(output, {recursive:true});
let configured = false, loggedIn = false;
let networkRevision = 0, additionalOrigins = [], confirmedAt = null, checkLifetime = 600;
const accounts = [{id:1,username:'owner',name:'Александра',admin:true}];
const server = http.createServer(async (req,res) => {
  const url = new URL(req.url, 'http://localhost');
  if (url.pathname.startsWith('/companion-api/')) {
    let bytes = ''; for await (const part of req) bytes += part;
    const form = JSON.parse(bytes || '{}');
    const route = url.pathname.slice('/companion-api/'.length);
    let data = {}, status = 200;
    if (route === 'status') data = {configured};
    else if (route === 'network/confirm') { confirmedAt = Math.floor(Date.now()/1000); data = {confirmed:true}; }
    else if (route === 'setup' || route === 'login') {
      if (route === 'setup') assert.equal(req.headers['x-setup-key'], 'fixture-key');
      assert.equal(form.password, 'fixture-password');
      configured = loggedIn = true; data = {csrf:'fixture',user:accounts[0],readerReady:true};
    } else if (!loggedIn) { status = 401; data = {error:'Войдите в Companion.'}; }
    else if (route === 'me') data = {csrf:'fixture',user:accounts[0]};
    else if (route === 'network') {
      if (req.method === 'POST') {additionalOrigins = form.additionalOrigins; networkRevision++;}
      data = {primaryOrigin:`http://${req.headers.host}`,additionalOrigins,revision:networkRevision,publishedBind:'127.0.0.1',publishedPort:8080,externalReachability:'unverified'};
    }
    else if (route === 'network/external-ip') data = {address:'203.0.113.42',provider:'ipify',checkedAt:Math.floor(Date.now()/1000),externalReachability:'unverified'};
    else if (route === 'network/check') {
      confirmedAt = null;
      const checkUrl = `http://${req.headers.host}/connection-check#${'x'.repeat(43)}`;
      const qrDataUrl = execFileSync(process.env.PYTHON || 'python', ['-c',
        'import sys; from octofox_library.qr import qr_data_url; print(qr_data_url(sys.argv[1]))', checkUrl],
        {encoding:'utf8',windowsHide:true,env:{...process.env,PYTHONPATH:path.resolve('src')}}).trim();
      data = {token:'x'.repeat(43),url:checkUrl,qrDataUrl,expiresIn:checkLifetime};
    }
    else if (route === 'network/check-status') data = {confirmedAt,scope:'browser',externalReachability:'unverified'};
    else if (route === 'users' && req.method === 'GET') data = {users:accounts};
    else if (route === 'users') {
      accounts.push({id:accounts.length+1,username:form.username,name:form.name,admin:false});
      status = 201; data = {readerReady:true};
    } else if (route === 'reader-access') data = {readerReady:true};
    else if (route === 'logout') loggedIn = false;
    else status = 404;
    res.writeHead(status, {'Content-Type':'application/json'}); return res.end(JSON.stringify(data));
  }
  const relative = url.pathname === '/companion' ? 'companion.html' : url.pathname === '/connection-check' ? 'connection-check.html' : url.pathname.slice(1);
  const filename = path.resolve(root, relative);
  if (!filename.startsWith(root + path.sep) || !fs.existsSync(filename)) {res.writeHead(404); return res.end();}
  const mime = {'.html':'text/html; charset=utf-8','.js':'text/javascript; charset=utf-8','.css':'text/css','.woff2':'font/woff2','.svg':'image/svg+xml'}[path.extname(filename)];
  res.writeHead(200, {'Content-Type':mime || 'application/octet-stream'}); res.end(readAsset(filename));
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
    await page.getByRole('heading',{name:english ? 'Sign in to Companion' : 'Войти в Companion'}).waitFor();
    assert.equal(await page.locator('#setup-fields').isVisible(), false);
    await form.locator('[name="username"]').fill('owner');
    await form.locator('[name="password"]').fill('fixture-password');
    await form.locator('button').click();
    await page.locator('#dashboard').waitFor({state:'visible'});
    await page.reload(); await page.locator('#dashboard').waitFor({state:'visible'});
    await page.locator('#network-tab').click();
    await page.locator('#primary-origin').filter({hasText:'http://'}).waitFor();
    await page.locator('#additional-origins').fill('http://192.168.1.20:8080\nhttps://books.example.org');
    await page.locator('#network-form button').click();
    await page.getByText(english ? 'Addresses saved.' : 'Адреса сохранены.', {exact:false}).waitFor();
    await page.locator('#external-ip').click();
    await page.locator('#external-ip-result').filter({hasText:'203.0.113.42'}).waitFor();
    await page.locator('.cloudflare-guide summary').click();
    for (const width of [393,768,1024,1280]) {
      await page.setViewportSize({width,height:980});
      assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), `network overflow at ${width}`);
    }
    await page.screenshot({path:path.join(output,'network-desktop.png'),fullPage:true});
    await page.locator('#connection-form button').click();
    await page.locator('#check-link-panel').waitFor({state:'visible'});
    await page.locator('#check-qr').evaluate(img => img.decode());
    await page.locator('#check-qr').screenshot({path:path.join(output,'connection-qr.png')});
    fs.writeFileSync(path.join(output,'connection-qr-url.txt'), await page.locator('#check-url').inputValue());
    await page.screenshot({path:path.join(output,'network-qr-desktop.png'),fullPage:true});
    const opened = page.waitForEvent('popup');
    await page.locator('#open-check').click();
    const checkPage = await opened;
    await checkPage.getByText(english ? 'Connection confirmed.' : 'Подключение подтверждено.', {exact:false}).waitFor();
    await checkPage.screenshot({path:path.join(output,'connection-confirmed.png'),fullPage:true});
    await checkPage.close();
    await page.locator('#check-refresh').click();
    await page.locator('#check-status').filter({hasText:english ? 'Address opened' : 'Адрес открыт'}).waitFor();
    await page.setViewportSize({width:393,height:852});
    assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'QR mobile overflow');
    await page.screenshot({path:path.join(output,'network-mobile.png'),fullPage:true});
    await page.locator('#check-qr').screenshot({path:path.join(output,'connection-qr-mobile.png')});
    checkLifetime = 1;
    await page.locator('#connection-form button').click();
    await page.locator('#network-notice').filter({hasText:english ? 'This QR code has expired' : 'Срок QR-кода истёк'}).waitFor();
    assert.equal(await page.locator('#check-link-panel').isVisible(), false);
    assert.equal(await page.locator('#check-qr').getAttribute('src'), null);
    await page.locator('#readers-tab').click();
    await page.locator('#dashboard').waitFor({state:'visible'});
    assert.deepEqual(errors, []);
    console.log('Companion UI passed: accounts, network settings, IP lookup, Cloudflare guide, local QR, connection confirmation and QR expiry; widths 393–1280.');
  } finally {await browser.close(); await new Promise(resolve => server.close(resolve));}
})().catch(error => {console.error(error); process.exitCode = 1; server.close();});
