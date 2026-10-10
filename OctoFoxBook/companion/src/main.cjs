'use strict';
const {app,BrowserWindow,WebContentsView,ipcMain,session,shell,dialog}=require('electron');
const path=require('node:path'),fs=require('node:fs'),crypto=require('node:crypto');
const {pathToFileURL}=require('node:url');
const discovery=require('./discovery.cjs');
const localPage=pathToFileURL(path.join(__dirname,'shell.html')).href;
let window,remote,active,scanning=false,records=new Map(),generation=0,refresh;
const hidden=process.env.OCTOFOX_TEST_HIDE==='1';
if(process.env.OCTOFOX_TEST_PROFILE)app.setPath('userData',process.env.OCTOFOX_TEST_PROFILE);
function settings(){try{return JSON.parse(fs.readFileSync(path.join(app.getPath('userData'),'library.json'),'utf8'));}catch{return {};}}
function remember(server){fs.writeFileSync(path.join(app.getPath('userData'),'library.json'),JSON.stringify({id:server.id,origin:server.origin}),{mode:0o600});}
function publicRecord(s){return {id:s.id,name:s.name,origin:s.origin,configured:s.configured};}
function status(data){if(window&&!window.isDestroyed())window.webContents.send('companion:status',data);}
function resize(){if(remote){const [width,height]=window.getContentSize();remote.setBounds({x:0,y:64,width,height:Math.max(0,height-64)});}}
function closeRemote(){generation++;clearInterval(refresh);if(remote){window.contentView.removeChildView(remote);remote.webContents.close();remote=null;}active=null;}
function trusted(event){if(event.sender!==window?.webContents||event.senderFrame?.url!==localPage)throw Error('Unavailable.');}
function register(name,fn){ipcMain.handle('companion:'+name,async(event,...args)=>{trusted(event);try{return await fn(...args);}catch(error){return {error:error.message||'Could not connect. Try again.'};}});}
async function readerRequest(action,data){
 if(!remote||!active)throw Error('Connect to your library first.');
 const routes={info:'/companion-api/readers',create:'/companion-api/readers',revoke:'/companion-api/readers/revoke'};
 if(!Object.hasOwn(routes,action))throw Error('Unknown reader action.');
 const run=generation,origin=active.origin,isolated=remote.webContents.session;
 const headers={Origin:origin};
 if(active.ticket)headers['X-OctoFox-Native']=active.ticket;
 const me=await isolated.fetch(origin+'/companion-api/me',{credentials:'include',headers});
 if(!me.ok)throw Error('Sign in to Companion as the library owner first.');
 const session=await me.json();
 if(run!==generation)throw Error('The library connection changed.');
 const body=action==='info'?undefined:JSON.stringify(data);
 if(body&&body.length>4096)throw Error('Pairing request is too large.');
 const result=await isolated.fetch(origin+routes[action],{credentials:'include',method:body?'POST':'GET',
  headers:{...headers,'Content-Type':'application/json','X-CSRF-Token':session.csrf},body});
 const value=await result.json();
 if(!result.ok)throw Error(value.error||'Could not pair the reader.');
 if(run!==generation)throw Error('The library connection changed.');
 return value;
}
async function connect(id){
 const server=records.get(id);if(!server)throw Error('Search again to find this library.');
 closeRemote();const run=generation;active={...server};status({phase:'connecting',server:publicRecord(server)});
 const partition='persist:library-'+crypto.createHash('sha256').update(server.id+'|'+server.origin).digest('hex');
 const isolated=session.fromPartition(partition);
 await isolated.setProxy({mode:server.ticket?'direct':'system'});
 isolated.setPermissionRequestHandler((_wc,_permission,callback)=>callback(false));
 isolated.setPermissionCheckHandler(()=>false);
 isolated.webRequest.onBeforeSendHeaders((details,callback)=>{
  const headers={...details.requestHeaders};for(const key of Object.keys(headers))if(key.toLowerCase()==='x-octofox-native')delete headers[key];
  if(active?.origin===new URL(details.url).origin&&active.ticket)headers['X-OctoFox-Native']=active.ticket;
  callback({requestHeaders:headers});
 });
 remote=new WebContentsView({webPreferences:{session:isolated,nodeIntegration:false,contextIsolation:true,sandbox:true,backgroundThrottling:false,webSecurity:true,navigateOnDragDrop:false}});
 window.contentView.addChildView(remote);resize();
 const contents=remote.webContents;
 contents.setWindowOpenHandler(({url})=>{
  // Same-origin download/check tabs remain in the application. External help is
  // opened only in the system browser; the embedded page has no native bridge.
  try{const target=new URL(url);if(target.origin===server.origin){contents.loadURL(url);return {action:'deny'};}
   if(target.protocol==='https:'&&!target.username&&!target.password&&['dash.cloudflare.com','developers.cloudflare.com','github.com'].includes(target.hostname))shell.openExternal(url).catch(()=>{});
  }catch{}return {action:'deny'};
 });
 const navigation=(event,url)=>{if(new URL(url).origin!==server.origin)event.preventDefault();};
 contents.on('will-navigate',navigation);contents.on('will-redirect',navigation);
 contents.on('did-fail-load',(_event,code,_description,_url,main)=>{
  if(main&&code!==-3&&run===generation){closeRemote();status({phase:'error',message:'The library stopped responding. Check that the server is running, then search again.'});}
 });
 contents.on('render-process-gone',()=>{if(run===generation){closeRemote();status({phase:'error',message:'The library window closed unexpectedly. Search again to reconnect.'});}});
 await contents.loadURL(server.origin+'/companion');
 if(run!==generation)return {};
 remember(server);status({phase:'connected',server:publicRecord(server)});
 if(server.ticket)refresh=setInterval(async()=>{
  try{const candidates=await discovery.scan({hosts:[new URL(server.origin).hostname]});const updated=candidates.find(s=>s.id===server.id&&s.origin===server.origin);if(updated&&run===generation){active.ticket=updated.ticket;records.set(updated.id,updated);}}catch{}
 },20*60*1000);
 return {};
}
async function scan(){
 if(scanning)return {};scanning=true;closeRemote();const run=generation;status({phase:'scanning'});
 try{
  const last=settings();
  if(last.origin)try{
   const origin=discovery.address(last.origin),host=new URL(origin).hostname;
   const nearby=discovery.localIPv4(host)?await discovery.scan({hosts:[host],duration:450}):[];
   const previous=nearby.find(s=>s.id===last.id)||(origin.startsWith('https:')?await discovery.probe(origin):null);
   if(previous?.id===last.id&&run===generation){records=new Map([[previous.id,previous]]);return await connect(previous.id);}
  }catch{}
  const found=await discovery.scan(hidden&&process.env.OCTOFOX_TEST_LOCAL==='1'?{hosts:['127.0.0.1']}:{});
  if(last.origin&&!found.some(s=>s.id===last.id))try{const saved=await discovery.probe(discovery.address(last.origin));if(saved.id===last.id)found.push(saved);}catch{}
  if(run!==generation)return {};records=new Map(found.map(s=>[s.id,s]));
  const lastFound=found.find(s=>s.id===last.id);
  if(lastFound||found.length===1)return await connect((lastFound||found[0]).id);
  status({phase:found.length?'choose':'empty',servers:found.map(publicRecord)});return {};
 }finally{scanning=false;}
}
app.whenReady().then(()=>{
 fs.mkdirSync(app.getPath('userData'),{recursive:true,mode:0o700});
 window=new BrowserWindow({width:1180,height:860,minWidth:680,minHeight:540,show:!hidden,backgroundColor:'#100d10',title:'OctoFox Companion',autoHideMenuBar:true,icon:path.join(__dirname,'../assets/icon.png'),
  webPreferences:{preload:path.join(__dirname,'preload.cjs'),nodeIntegration:false,contextIsolation:true,sandbox:true,backgroundThrottling:false,webSecurity:true,offscreen:hidden}});
 window.webContents.setWindowOpenHandler(()=>({action:'deny'}));
 const localSession=window.webContents.session;
 localSession.setPermissionCheckHandler((wc,permission)=>wc===window?.webContents&&permission==='serial'&&wc.getURL()===localPage);
 localSession.setPermissionRequestHandler((wc,permission,callback)=>callback(wc===window?.webContents&&permission==='serial'&&wc.getURL()===localPage));
 localSession.setDevicePermissionHandler(details=>details.deviceType==='serial'&&details.origin==='file://');
 localSession.on('select-serial-port',(event,ports,wc,callback)=>{
  event.preventDefault();
  if(wc!==window?.webContents||wc.getURL()!==localPage){callback('');return;}
  const matches=ports.filter(p=>parseInt(p.vendorId,16)===0x303a&&parseInt(p.productId,16)===0x1001);
  // Do not guess between two physical readers. The OS chooser is filtered to
  // the supported USB identity, and the firmware is checked again over serial.
  callback(matches.length===1?matches[0].portId:'');
 });
 window.webContents.on('will-navigate',event=>event.preventDefault());
 window.on('resize',resize);window.on('closed',()=>{clearInterval(refresh);if(remote&&!remote.webContents.isDestroyed())remote.webContents.close();remote=null;window=null;});
 register('scan',scan);register('connect',connect);
 register('reader',readerRequest);
 register('pair-panel',visible=>{
  if(typeof visible==='boolean'&&remote){
   if(visible)window.contentView.removeChildView(remote);
   else {window.contentView.addChildView(remote);resize();}
  }
  return {};
 });
 register('manual',async value=>{if(typeof value!=='string'||value.length>300)throw Error('Enter a library address.');const origin=discovery.address(value),host=new URL(origin).hostname;
  const local=discovery.localIPv4(host)||host==='localhost';
  const found=local?await discovery.scan({hosts:[host==='localhost'?'127.0.0.1':host]}):[];
  const server=found.find(s=>s.origin===origin)||await discovery.probe(origin);
  if(!server.configured&&!server.desktopSetup)throw Error('Automatic setup needs a direct local connection. Allow OctoFox discovery (UDP 49645) on the server, then search again.');
  records.set(server.id,server);return connect(server.id);});
 register('disconnect',()=>{closeRemote();status({phase:'choose',servers:[...records.values()].map(publicRecord)});return {};});
 register('navigate',where=>{if(!['companion','library'].includes(where)||!remote)return {};return remote.webContents.loadURL(active.origin+(where==='companion'?'/companion':'/#personal')).then(()=>({}));});
 window.loadFile(path.join(__dirname,'shell.html'));
});
app.on('window-all-closed',()=>app.quit());
