'use strict';
const dgram=require('node:dgram'),os=require('node:os'),crypto=require('node:crypto'),net=require('node:net');
const PRODUCT='octofox-library',PORT=49645;
function localIPv4(value){
 if(net.isIP(value)!==4)return false;
 const [a,b]=value.split('.').map(Number);
 return a===127||a===10||a===192&&b===168||a===172&&b>=16&&b<=31||a===169&&b===254;
}
function toNumber(ip){return ip.split('.').reduce((n,b)=>(n*256+Number(b))>>>0,0);}
function fromNumber(n){return [24,16,8,0].map(s=>(n>>>s)&255).join('.');}
function targets(interfaces=os.networkInterfaces()){
 const result=new Set(['127.0.0.1']);
 for(const entries of Object.values(interfaces))for(const nic of entries||[]){
  if(!localIPv4(nic.address)||nic.internal||nic.family!=='IPv4')continue;
  const ip=toNumber(nic.address),mask=toNumber(nic.netmask);
  result.add(nic.address);result.add(fromNumber((ip|~mask)>>>0));
  // One service port, at most the nearby /24 per adapter. Never scan the Internet.
  const bounded=(mask|0xffffff00)>>>0,first=(ip&bounded)>>>0,last=(ip|~bounded)>>>0;
  for(let n=first+1;n<last&&result.size<1024;n++)result.add(fromNumber(n));
 }
 return [...result].filter(localIPv4);
}
function address(value){
 const url=new URL(value.includes('://')?value:'http://'+value);
 if(!['http:','https:'].includes(url.protocol)||url.username||url.password||url.hash||url.search||!['','/','/companion'].includes(url.pathname))throw Error('Enter a library address without a username, password or extra path.');
 if(url.protocol==='http:'&&url.hostname!=='localhost'&&!localIPv4(url.hostname))throw Error('Use HTTPS for a library outside your local network.');
 return url.origin;
}
function validMetadata(data){return data?.product===PRODUCT&&data.protocol===1&&/^[a-f0-9-]{36}$/.test(data.instanceId)&&typeof data.name==='string'&&data.name.length<=80&&Number.isInteger(data.port)&&data.port>0&&data.port<=65535;}
async function probe(origin,ticket){
 const headers=ticket?{'X-OctoFox-Native':ticket}:{};
 const response=await fetch(origin+'/companion-api/status',{headers,redirect:'error',signal:AbortSignal.timeout(4500)});
 if(!response.ok)throw Error('This library could not be opened. Check its address or update the server.');
 const reader=response.body.getReader();let length=0;const chunks=[];
 try{while(true){const {done,value}=await reader.read();if(done)break;length+=value.length;if(length>4096)throw Error('Not an OctoFox server.');chunks.push(value);}}finally{await reader.cancel();}
 const text=Buffer.concat(chunks).toString('utf8');
 const data=JSON.parse(text);if(!validMetadata(data)||typeof data.configured!=='boolean')throw Error('Update this OctoFox server to use the desktop Companion.');
 return {id:data.instanceId,instanceId:data.instanceId,name:data.name,origin,configured:data.configured,ticket:ticket||'',desktopSetup:data.desktopSetup===true};
}
async function scan({duration=1800,hosts=targets(),port=PORT}={}){
 const socket=dgram.createSocket('udp4'),nonce=crypto.randomBytes(16).toString('hex'),found=new Map(),requested=new Set();
 const message=extra=>Buffer.from(JSON.stringify({product:PRODUCT,protocol:1,nonce,...extra}));
 await new Promise((resolve,reject)=>{socket.once('error',reject);socket.bind(0,'0.0.0.0',resolve);});
 socket.removeAllListeners('error');socket.on('error',()=>{});socket.setBroadcast(true);
 socket.on('message',(bytes,peer)=>{
  if(bytes.length>2048||!localIPv4(peer.address)||peer.port!==port)return;
  try{
   const data=JSON.parse(bytes);if(!validMetadata(data)||data.nonce!==nonce)return;
   const origin=`http://${peer.address}:${data.port}`;
   if(data.ticket){
    if(!/^[A-Za-z0-9_-]{43}$/.test(data.ticket)||!requested.has(origin))return;
    const previous=found.get(data.instanceId);
    if((previous||found.size<32)&&(!previous||peer.address.startsWith('127.')))found.set(data.instanceId,{origin,ticket:data.ticket});
   }else if(!requested.has(origin)){
    requested.add(origin);socket.send(message({action:'connect',origin}),port,peer.address,()=>{});
   }
  }catch{}
 });
 try{
  for(const host of hosts.filter(localIPv4))socket.send(message({action:'discover'}),port,host,()=>{});
  await new Promise(r=>setTimeout(r,duration));
 }finally{socket.close();}
 const results=await Promise.allSettled([...found.entries()].map(async([id,{origin,ticket}])=>{const server=await probe(origin,ticket);if(server.id!==id)throw Error('Server identity changed during discovery.');return server;}));
 return results.filter(r=>r.status==='fulfilled').map(r=>r.value);
}
module.exports={PRODUCT,PORT,localIPv4,targets,address,validMetadata,probe,scan};
