// Local fixture only: no live accounts, reading progress or audio jobs are changed.
const fs = require('node:fs'), path = require('node:path'), http = require('node:http');
const assert = require('node:assert/strict');
const {english, readAsset} = require('./localized-assets.cjs');
const {chromium, webkit} = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const root = path.resolve('src/octofox_library/web');
const output = path.resolve(process.env.QA_OUTPUT || 'test-results/browser'+(process.env.TEST_WEBKIT ? '-webkit' : ''));
fs.mkdirSync(output, {recursive:true});
const paragraph = 'Вечерний свет ложился на страницы открытой книги. За окном тихо шумели деревья, и путешествие продолжалось. Каждый новый поворот дороги обещал встречу, которую невозможно было предугадать. ';
const chapters = ['Первая встреча', 'Письмо', 'Дорога домой'];
const html = chapters.map((title, chapter) => `<h2>${title}</h2>` + Array.from({length:35}, (_,i) => `<p>${i + 1}. ${paragraph.repeat(2 + chapter)}</p>`).join(''));
const books = Array.from({length:18}, (_,i) => ({id:String(i+1), title:['Тихий свет','Письма издалека','Сад за облаками','Дорога домой','Море и звёзды','Вечерняя библиотека'][i%6], author:'Автор книги', reading:{chapter:0,offset:0,shelf:''}, inLibrary:true, cover:''}));
const server = http.createServer((req,res) => {
  const url=new URL(req.url,'http://localhost');
  if (url.pathname.startsWith('/reader-api/')) {
    let data = {};
    if (url.pathname.endsWith('/me')) data={csrf:'fixture',username:'Проверка',devices:[],flibustaAdmin:false};
    else if (url.pathname.endsWith('/devices') || url.pathname.endsWith('/queue')) data=[];
    else if (url.pathname.endsWith('/catalog-status')) data={status:'ready',revision:1};
    else if (url.pathname.endsWith('/personal-collections')) data={collections:[]};
    else if (url.pathname.endsWith('/books')) data={books,total:books.length,more:false,indexStatus:'ready',catalogRevision:1};
    else if (url.pathname.endsWith('/read')) {
      const chapter=Number(url.searchParams.get('chapter')||0);
      data=url.searchParams.has('start') ? {total:3,next:3,items:html.slice(Number(url.searchParams.get('start'))).map((h,i)=>({chapter:Number(url.searchParams.get('start'))+i,html:h}))} : {chapter,chapters,html:html[chapter]};
    } else if (/\/books\/\d+$/.test(url.pathname)) data={...books[0],summary:'История о книгах, путешествиях и неожиданных встречах.',authors:['Автор книги']};
    else if (url.pathname.endsWith('/speech')) data={available:false,voices:[]};
    res.writeHead(200,{'Content-Type':'application/json'}); res.end(JSON.stringify(data)); return;
  }
  const file=path.resolve(root,'.'+(url.pathname==='/'?'/index.html':url.pathname));
  if (!file.startsWith(root+path.sep) || !fs.existsSync(file) || !fs.statSync(file).isFile()) {res.writeHead(404);res.end();return;}
  res.setHeader('Content-Type',file.endsWith('.js')?'text/javascript':file.endsWith('.css')?'text/css':file.endsWith('.html')?'text/html':'application/octet-stream');
  res.end(readAsset(file));
});
(async()=>{
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  const browser=await (process.env.TEST_WEBKIT ? webkit.launch({headless:true}) : chromium.launch({headless:true}));
  try {
    const page=await browser.newPage({viewport:{width:1194,height:834},deviceScaleFactor:1,hasTouch:true});
    const errors=[];page.on('pageerror',e=>errors.push(e.message));
    await page.goto(`http://127.0.0.1:${server.address().port}/#personal`);
    await page.waitForSelector('.book-tile');
    assert(await page.locator('#drawer').evaluate(n=>!n.inert));
    assert(await page.locator('#menuButton').isHidden());
    await page.screenshot({path:path.join(output,'ipad-library.png')});
    for (const [width,height,columns] of [[768,1024,3],[810,1080,3],[834,1194,3],[1024,1366,3],[1024,768,4],[1194,834,4],[1366,1024,5]]) {
      await page.setViewportSize({width,height});
      await page.waitForFunction(n=>getComputedStyle(document.querySelector('.book-grid')).gridTemplateColumns.split(' ').length===n,columns);
      assert(await page.locator('#drawer').evaluate(n=>!n.inert));
      assert(await page.locator('#menuButton').isHidden());
      assert(await page.evaluate(()=>{
        const sidebar=$('drawer').getBoundingClientRect(), grid=document.querySelector('.book-grid').getBoundingClientRect();
        return sidebar.left===0 && grid.left>=sidebar.right && grid.right<=innerWidth && document.documentElement.scrollWidth<=innerWidth;
      }),`Sidebar/grid fit ${width}x${height}`);
      if(width===834) await page.screenshot({path:path.join(output,'ipad-portrait-library.png')});
    }
    await page.setViewportSize({width:600,height:1024});
    await page.waitForFunction(()=>$('drawer').inert && $('drawer').getBoundingClientRect().right<=0);
    assert(await page.locator('#menuButton').isVisible());
    await page.click('#menuButton');
    assert(await page.locator('#drawer').evaluate(n=>!n.inert && n.classList.contains('open')));
    await page.setViewportSize({width:834,height:1194});
    await page.waitForFunction(()=>!$('drawer').inert && !$('drawer').classList.contains('open') && $('drawerBackdrop').hidden && !document.body.classList.contains('locked'));
    await page.setViewportSize({width:1194,height:834});
    await page.evaluate(()=>openBook('1'));
    await page.screenshot({path:path.join(output,'ipad-book.png')});
    await page.click('#readBook');
    await page.waitForFunction(()=>state.reader?.pages?.columns===2 && state.reader.bookPages.position(0,0));
    await page.screenshot({path:path.join(output,'ipad-spread.png')});
    const result=await page.evaluate(async()=>{
      const p=state.reader.pages;
      const left=p.bounds(0), right=p.bounds(1);
      followReaderAnchor(right.anchor);
      if (p.page!==1 || p.spreadStart!==0) throw Error('Speech moved the spread at its right page');
      const saved=readerPagePosition();
      if (JSON.stringify(saved.anchor)!==JSON.stringify(right.anchor)) throw Error('Wrong saved right-hand anchor');
      const speech=speechPage();
      if (speech.page!==1) throw Error('Audio starts at wrong page');
      const next=await nextSpeechPage(speech);
      if(next.page!==2 || JSON.stringify(next.anchor)!==JSON.stringify(p.bounds(2).anchor)) throw Error('Audio lookahead differs from rendered pagination');
      await turnReaderPage(1);
      if(p.page!==2 || p.spreadStart!==2) throw Error('Manual turn must advance a spread');
      if(document.querySelectorAll('.reader-turn-sheet').length!==1) throw Error('Missing flip animation');
      const real=anchorRange(p.firstAnchor()).getBoundingClientRect(), viewport=$('readerScroll').getBoundingClientRect();
      if(real.left<viewport.left || real.right>viewport.right) throw Error('Animation corrupted text range geometry');
      const ranges=Array.from({length:p.count},(_,i)=>p.bounds(i));
      for(let i=1;i<ranges.length;i++) if(JSON.stringify(ranges[i-1].end)!==JSON.stringify(ranges[i].anchor)) throw Error('Lost text at page boundary');
      return {pages:p.count,columns:p.columns,anchor:readerPagePosition().anchor,left,right};
    });
    await page.waitForFunction(()=>!document.querySelector('.reader-turn-sheet'));
    await page.evaluate(async()=>{
      const viewport=$('readerScroll'), start=state.reader.pages.spreadStart;
      viewport.onpointerdown({isPrimary:true,button:0,clientX:800,clientY:200,target:viewport});
      await viewport.onpointerup({pointerType:'touch',clientX:600,clientY:200,target:viewport});
      if(state.reader.pages.spreadStart!==start+2) throw Error('Tablet swipe did not advance one spread');
    });
    // Reflow keeps the chosen text in view, including when opening settings.
    const anchor=await page.evaluate(()=>({...state.reader.anchor}));
    await page.setViewportSize({width:834,height:1194});
    await page.waitForFunction(()=>state.reader.pages.columns===1 && !state.reader.layoutPending);
    assert.deepEqual(await page.evaluate(()=>({...state.reader.anchor})),anchor);
    await page.screenshot({path:path.join(output,'ipad-portrait.png')});
    await page.setViewportSize({width:1194,height:834});
    await page.waitForFunction(()=>state.reader.pages.columns===2 && !state.reader.layoutPending);
    assert.deepEqual(await page.evaluate(()=>({...state.reader.anchor})),anchor);
    await page.evaluate(async()=>{state.reader.pages.show(state.reader.pages.count-1);await turnReaderPage(1);});
    assert.equal(await page.evaluate(()=>state.reader.chapter),1);
    await page.evaluate(()=>turnReaderPage(-1));
    assert.equal(await page.evaluate(()=>state.reader.chapter),0);
    await page.emulateMedia({reducedMotion:'reduce'});
    await page.evaluate(()=>turnReaderPage(-1));
    assert.equal(await page.locator('.reader-turn-sheet').count(),0);
    for (const size of [{width:1024,height:768},{width:1366,height:1024},{width:768,height:1024},{width:600,height:1024}]) {
      await page.setViewportSize(size);
      const columns=size.width>=1000 ? 2 : 1;
      await page.waitForFunction(n=>state.reader.pages.columns===n && !state.reader.layoutPending,columns);
      assert(await page.evaluate(()=>{
        const v=$('readerScroll').getBoundingClientRect(), f=document.querySelector('.reader-bottom').getBoundingClientRect();
        return v.bottom<=f.top+1 && f.bottom<=innerHeight+1 && v.right<=innerWidth;
      }), 'Text viewport must fit above footer at every tablet size');
    }
    await page.setViewportSize({width:393,height:852});
    await page.waitForFunction(()=>state.reader.pages.columns===1 && !state.reader.layoutPending);
    // ResizeObserver runs after setViewportSize; one-column mode may already be
    // true from the previous viewport while the article still has its old width.
    await page.waitForFunction(()=>Math.abs(parseFloat($('readerText').style.width)
      - ($('readerScroll').clientWidth / state.reader.pages.columns - 44)) < 1);
    await page.waitForFunction(()=>/^(?:Стр\.|Page) \d/.test($('readerPage').textContent));
    await page.screenshot({path:path.join(output,'iphone-reader.png')});
    await page.evaluate(()=>closeReader());
    await page.evaluate(()=>{if($('bookDialog').open)closeBook();});
    await page.screenshot({path:path.join(output,'iphone-library.png')});
    assert(await page.locator('#menuButton').isVisible());
    assert(await page.locator('#drawer').evaluate(n=>n.inert));
    assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
    assert.deepEqual(errors,[]);
    console.log(JSON.stringify({ok:true,...result,screenshots:output}));
  } finally {await browser.close();server.close();}
})().catch(e=>{console.error(e);server.close();process.exitCode=1;});
