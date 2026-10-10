// Native Electron smoke journey with a disposable local discovery/server fixture.
const {_electron:electron}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
const assert=require('node:assert/strict'),http=require('node:http'),dgram=require('node:dgram'),fs=require('node:fs'),os=require('node:os'),path=require('node:path');
const output=path.resolve(process.env.QA_OUTPUT||'test-results/app');fs.mkdirSync(output,{recursive:true});
const profile=fs.mkdtempSync(path.join(os.tmpdir(),'octofox-app-'));
const id='00112233-4455-4677-8899-aabbccddeeff',ticket='s'.repeat(43);let configured=false;
const web=http.createServer((req,res)=>{
 if(req.url==='/companion-api/me'){
  assert.match(req.headers.cookie||'',/fixture=ready/);
  assert.equal(req.headers.origin,'http://127.0.0.1:'+web.address().port);
  res.setHeader('Content-Type','application/json');return res.end(JSON.stringify({csrf:'pair-test',user:{username:'owner'}}));
 }
 if(req.url==='/companion-api/readers'){
  res.setHeader('Content-Type','application/json');
  return res.end(JSON.stringify({currentAccount:'owner',origins:['http://127.0.0.1','https://books.example'],readers:[{username:'fixture-key',owner:'Reader account',device:'reader-001122334455',revoked:false}]}));
 }
 if(req.url==='/companion-api/status'){res.setHeader('Content-Type','application/json');return res.end(JSON.stringify({product:'octofox-library',protocol:1,instanceId:id,name:'My home library',port:web.address().port,configured,desktopSetup:req.headers['x-octofox-native']===ticket}));}
 res.setHeader('Content-Type','text/html');
 res.setHeader('Set-Cookie','fixture=ready; Path=/companion-api; HttpOnly; SameSite=Strict');
 res.end('<!doctype html><html lang="en"><head><title>OctoFox test server</title></head><body><h1>Server interface</h1><p id="native">'+(req.headers['x-octofox-native']===ticket?'Automatic setup ready':'No setup handshake')+'</p><input name="username"><button id="action">Create owner</button><a href="https://example.org/" id="escape">Leave server</a><script>document.querySelector("#action").onclick=()=>{document.querySelector("h1").textContent="Library ready"}</script></body></html>');
});
const udp=dgram.createSocket('udp4');
udp.on('message',(bytes,peer)=>{try{const q=JSON.parse(bytes);udp.send(Buffer.from(JSON.stringify({product:'octofox-library',protocol:1,instanceId:id,name:'My home library',port:web.address().port,nonce:q.nonce,...(q.action==='connect'?{ticket}:{})})),peer.port,peer.address);}catch{}});
(async()=>{let desktop;try{
 await new Promise(r=>web.listen(0,'127.0.0.1',r));await new Promise(r=>udp.bind(49645,'127.0.0.1',r));
 desktop=await electron.launch({executablePath:require('electron'),args:[path.resolve(__dirname,'..')],env:{...process.env,OCTOFOX_TEST_HIDE:'1',OCTOFOX_TEST_LOCAL:'1',OCTOFOX_TEST_PROFILE:profile}});
 const shell=await desktop.firstWindow();
 await shell.locator('#connected').waitFor({state:'visible',timeout:20000});
 let remote;for(let i=0;i<40;i++){remote=desktop.context().pages().find(p=>p.url().startsWith('http://127.0.0.1:'));if(remote)break;await new Promise(r=>setTimeout(r,100));}
 assert(remote,'embedded server page');assert.equal(await remote.locator('#native').innerText(),'Automatic setup ready');
 assert.deepEqual(await remote.evaluate(()=>[typeof require,typeof process,typeof window.companion]),['undefined','undefined','undefined']);
 await shell.click('#pair-reader');await shell.locator('#pair-panel').waitFor({state:'visible'});
 await shell.locator('#pair-origin option').waitFor({state:'attached'});
 assert.equal(await shell.locator('#pair-origin').inputValue(),'https://books.example');
 assert.equal(await shell.locator('#pair-submit').isDisabled(),true);
 assert.equal(await shell.locator('#pair-account').inputValue(),'owner');
 assert.equal(await shell.locator('#pair-password-row').isVisible(),false);
 assert.match(await shell.locator('#pair-keys').innerText(),/Reader account/);
 await shell.evaluate(()=>new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve))));
 await shell.screenshot({path:path.join(output,'pair-panel.png')});
 const pairingImage=await desktop.evaluate(async({BrowserWindow})=>(await BrowserWindow.getAllWindows()[0].capturePage()).toPNG().toString('base64'));
 fs.writeFileSync(path.join(output,'pair-reader.png'),Buffer.from(pairingImage,'base64'));
 await shell.click('#pair-close');await shell.locator('#pair-panel').waitFor({state:'hidden'});
 await remote.fill('[name=username]','new-owner');await remote.click('#action');assert.equal(await remote.locator('h1').innerText(),'Library ready');
 await remote.click('#escape',{noWaitAfter:true});assert(remote.url().startsWith('http://127.0.0.1:'));
 const image=await desktop.evaluate(async({BrowserWindow})=>{const shot=await BrowserWindow.getAllWindows()[0].capturePage();return shot.toPNG().toString('base64');});
 fs.writeFileSync(path.join(output,'connected.png'),Buffer.from(image,'base64'));
 await shell.click('#switch');await shell.locator('#servers button').waitFor();const picker=await desktop.evaluate(async({BrowserWindow})=>(await BrowserWindow.getAllWindows()[0].capturePage()).toPNG().toString('base64'));fs.writeFileSync(path.join(output,'choose-library.png'),Buffer.from(picker,'base64'));
 await shell.click('#servers button');await shell.locator('#connected').waitFor({state:'visible'});
 await desktop.close();desktop=null;configured=true;
 desktop=await electron.launch({executablePath:require('electron'),args:[path.resolve(__dirname,'..')],env:{...process.env,OCTOFOX_TEST_HIDE:'1',OCTOFOX_TEST_LOCAL:'1',OCTOFOX_TEST_PROFILE:profile}});
 await (await desktop.firstWindow()).locator('#connected').waitFor({state:'visible',timeout:20000});
 console.log('PASS: native window, automatic UDP connection, server interface, isolated remote renderer, navigation guard, switch and remembered reconnection.');
}finally{if(desktop)await desktop.close();udp.close();await new Promise(r=>web.close(r));}})().catch(e=>{console.error(e.message);process.exitCode=1;});
