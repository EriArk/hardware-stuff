const {test}=require('node:test'),assert=require('node:assert/strict');
const http=require('node:http'),dgram=require('node:dgram');
const {address,localIPv4,targets,probe,scan,PRODUCT}=require('../src/discovery.cjs');
test('discovery stays in local IPv4 networks and caps broad adapters',()=>{
 for(const ip of ['127.0.0.1','10.20.30.40','172.16.0.8','192.168.2.9'])assert(localIPv4(ip));
 for(const ip of ['8.8.8.8','100.64.0.2','172.32.0.1','0.0.0.0','::1'])assert(!localIPv4(ip));
 const hosts=targets({lan:[{family:'IPv4',address:'10.20.30.5',netmask:'255.0.0.0',internal:false}]});
 assert(hosts.length<260);assert(hosts.includes('10.20.30.200'));assert(!hosts.includes('10.20.40.200'));
});
test('manual address rejects unsafe schemes, credentials and public HTTP',()=>{
 assert.equal(address('192.168.1.20:8080'),'http://192.168.1.20:8080');
 assert.equal(address('https://books.example.org/companion'),'https://books.example.org');
 for(const url of ['file:///a','javascript:alert(1)','http://example.org','https://user:pw@example.org','https://example.org/a?token=x'])assert.throws(()=>address(url));
});
test('real UDP handshake validates nonce and server before exposing a record',async()=>{
 const identity='00112233-4455-4677-8899-aabbccddeeff',ticket='x'.repeat(43);
 let headers;
 const web=http.createServer((req,res)=>{headers=req.headers;res.setHeader('Content-Type','application/json');res.end(JSON.stringify({product:PRODUCT,protocol:1,instanceId:identity,name:'Fixture library',port:web.address().port,configured:false,desktopSetup:headers['x-octofox-native']===ticket}));});
 const udp=dgram.createSocket('udp4');
 await new Promise(r=>web.listen(0,'127.0.0.1',r));await new Promise(r=>udp.bind(0,'127.0.0.1',r));
 udp.on('message',(bytes,peer)=>{const q=JSON.parse(bytes);const response={product:PRODUCT,protocol:1,instanceId:identity,name:'Fixture library',port:web.address().port,nonce:q.nonce};if(q.action==='connect')response.ticket=ticket;udp.send(Buffer.from(JSON.stringify(response)),peer.port,peer.address);});
 try{const result=await scan({duration:150,hosts:['127.0.0.1'],port:udp.address().port});assert.equal(result.length,1);assert.equal(result[0].desktopSetup,true);assert.equal(headers['x-octofox-native'],ticket);}
 finally{udp.close();await new Promise(r=>web.close(r));}
});
test('server probe rejects redirects and oversized responses',async()=>{
 let mode='large';const web=http.createServer((_req,res)=>{if(mode==='redirect'){res.writeHead(302,{Location:'http://127.0.0.1:1/'});res.end();}else res.end('x'.repeat(5000));});
 await new Promise(r=>web.listen(0,'127.0.0.1',r));
 try{const origin='http://127.0.0.1:'+web.address().port;await assert.rejects(probe(origin));mode='redirect';await assert.rejects(probe(origin));}finally{await new Promise(r=>web.close(r));}
});
