const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
const {spawn}=require('node:child_process'),fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const root=path.resolve(__dirname,'..'),out=path.resolve(process.env.QA_OUTPUT||path.join(root,'test-results/ui'));fs.mkdirSync(out,{recursive:true});
const child=spawn(process.env.PYTHON||'python',[path.join(__dirname,'ui_fixture.py')],{cwd:root,windowsHide:true});
const url=new Promise((resolve,reject)=>{let text='';child.stdout.on('data',b=>{text+=b;if(text.includes('\n'))resolve(text.trim().split('\n')[0]);});child.once('error',reject);child.once('exit',c=>reject(Error('Fixture exited '+c)));});
(async()=>{let browser;try{
 const address=await url;browser=await chromium.launch({headless:true});const page=await browser.newPage({viewport:{width:1280,height:1000}});
 const errors=[];page.on('pageerror',e=>errors.push(e.message));await page.goto(address);await page.waitForFunction(()=>document.querySelector('#directory').value.length>0);
 await page.screenshot({path:path.join(out,'welcome-1280.png'),fullPage:true});
 for(const width of [393,768]){await page.setViewportSize({width,height:950});assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));await page.screenshot({path:path.join(out,`welcome-${width}.png`),fullPage:true});}
 await page.setViewportSize({width:1280,height:1000});await page.click('#check');await page.waitForFunction(()=>document.querySelector('#phase').textContent==='System components will be prepared');assert(await page.locator('#install').isEnabled());
 await page.fill('#directory','C:/OctoFox/blocked');await page.click('#install');await page.locator('#error').waitFor({state:'visible'});assert.match(await page.locator('#error').innerText(),/cancelled/);assert(await page.locator('#install').isEnabled());await page.screenshot({path:path.join(out,'retry.png'),fullPage:true});
 await page.fill('#directory','C:/OctoFox/server');await page.click('#install');await page.locator('#success').waitFor({state:'visible'});assert.equal(await page.locator('#installed-path').innerText(),'C:/OctoFox/server');assert(!await page.locator('#form').isVisible());await page.screenshot({path:path.join(out,'ready.png'),fullPage:true});
 await page.click('#finish');await page.waitForFunction(()=>document.querySelector('#finish').textContent==='All done');assert.deepEqual(errors,[]);
 console.log('Installer UI: welcome at 393/768/1280, dependency check, permission retry, successful handoff and finish passed.');
}finally{if(browser)await browser.close();child.kill();}})().catch(e=>{console.error(e);process.exitCode=1;});
