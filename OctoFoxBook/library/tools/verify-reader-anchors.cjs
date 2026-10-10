const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
const fs=require('node:fs'),assert=require('node:assert/strict'),{execFileSync}=require('node:child_process');
const code="import json;from test_reader_state import BOOK;from octofox_library.books_web import fb2_chapters;from octofox_library.reader_anchors import _Blocks;c=fb2_chapters(BOOK);print(json.dumps([{'html':x['html'],'blocks':[b['text'] for b in _Blocks(x['html']).blocks]} for x in c]))";
const chapters=JSON.parse(execFileSync(process.env.PYTHON||'python',['-c',code],{env:{...process.env,PYTHONPATH:'src;tests',PYTHONUTF8:'1'},encoding:'utf8'}));
const source=fs.readFileSync('src/octofox_library/web/app.js','utf8');
const fn=source.slice(source.indexOf('function readerTextBlocks('),source.indexOf('function anchorRange('));
(async()=>{const browser=await chromium.launch({executablePath:process.env.CHROMIUM_EXECUTABLE});try{
 const page=await browser.newPage();
 for(const chapter of chapters){await page.setContent('<article id="book">'+chapter.html+'</article>');
  const actual=await page.evaluate("(()=>{"+fn+";return readerTextBlocks(document.getElementById('book')).map(b=>b.text)})()");
  assert.deepEqual(actual,chapter.blocks);
 }
 console.log('PASS: server text-block mapping matches Chromium DOM, including headings, inline formatting, whitespace and emoji.');
}finally{await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
