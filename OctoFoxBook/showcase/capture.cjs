/* Reproducible screenshots of the presentation prototype. No external services. */
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const root = path.join(__dirname, 'demo');
const mime = {'.html':'text/html; charset=utf-8','.css':'text/css; charset=utf-8','.js':'text/javascript; charset=utf-8','.woff2':'font/woff2','.webp':'image/webp'};
const shots = [
 ['01-library-desktop','library',1440,1420,'Personal library'],
 ['02-book-details','book',1440,1000,'Book details and personal actions'],
 ['03-add-books','upload',1440,1160,'Batch import and extracted metadata'],
 ['04-collections','collections',1440,1050,'Personal collections'],
 ['05-reader-spread','reader',1440,1050,'Two-page reading'],
 ['06-reader-audio','audio',1440,1050,'Server narration and paragraph highlighting'],
 ['07-bookmarks','bookmarks',1440,1050,'Bookmarks independent of reading position'],
 ['08-device-shelf','devices',1440,1120,'Paired e-reader and managed shelf'],
 ['09-household','accounts',1440,960,'Personal household accounts'],
 ['10-settings-languages','settings',1440,960,'Language and reading preferences'],
 ['11-companion-domain-concept','companion',1440,1000,'Companion concept: home hosting and domain'],
 ['12-companion-pairing-concept','pairing',1440,1000,'Companion concept: reader pairing'],
 ['13-library-ipad-portrait','library',834,1194,'Portrait tablet library'],
 ['14-library-phone','library',393,852,'Phone library'],
 ['15-reader-phone','reader',393,852,'Phone reading'],
 ['16-sign-in','login',1440,1000,'Personal account sign-in']
];
const server = http.createServer((req,res)=>{
 const url = new URL(req.url,'http://localhost');
 let file;
 try { file=path.resolve(root,'.'+decodeURIComponent(url.pathname==='/'?'/index.html':url.pathname)); }
 catch { res.writeHead(400).end(); return; }
 if(!file.startsWith(root+path.sep)){res.writeHead(403).end();return;}
 fs.readFile(file,(err,data)=>{if(err){res.writeHead(404).end();return;}res.writeHead(200,{'Content-Type':mime[path.extname(file)]||'application/octet-stream','Cache-Control':'no-store'}).end(data);});
});
server.listen(process.argv.includes('--serve')?8876:0,'127.0.0.1',async()=>{
 const origin=`http://127.0.0.1:${server.address().port}`;
 if(process.argv.includes('--serve')){console.log(`Preview: ${origin}`);return;}
 let browser;
 try {
  const {webkit}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
  browser=await webkit.launch({headless:true});
  const out=path.join(__dirname,'screenshots');fs.mkdirSync(out,{recursive:true});
  const manifest=[];
  for(const [name,view,width,height,title] of shots){
   const page=await browser.newPage({viewport:{width,height},deviceScaleFactor:2});
   const errors=[];page.on('pageerror',e=>errors.push(e.message));
   await page.goto(`${origin}/?view=${view}`,{waitUntil:'networkidle'});
   await page.evaluate(()=>document.fonts.ready);
   const checks=await page.evaluate(()=>({text:document.body.innerText,overflow:document.documentElement.scrollWidth>innerWidth+1}));
   if(errors.length||checks.overflow||/[А-Яа-яЁё]/.test(checks.text))throw Error(`${name}: ${JSON.stringify({errors,overflow:checks.overflow,nonEnglish:/[А-Яа-яЁё]/.test(checks.text)})}`);
   await page.screenshot({path:path.join(out,name+'.png')});
   if(view==='library'){
    await page.locator('.search input').fill('Sherlock');
    if(await page.locator('.tile:visible').count()!==1)throw Error('Sample search failed');
    if(width<768){
     await page.locator('.mobile-menu').click();
     if(!(await page.locator('.sidebar').isVisible()))throw Error('Phone navigation failed');
    }
   }
   manifest.push({file:name+'.png',title,view,viewport:{width,height},scale:2,status:name.includes('concept')?'Companion concept':'Simulated website UI'});
   console.log(`PASS ${name}`);await page.close();
  }
  fs.writeFileSync(path.join(out,'manifest.json'),JSON.stringify(manifest,null,2)+'\n');
 }catch(err){console.error(err);process.exitCode=1;}
 finally{if(browser)await browser.close();server.close();}
});
