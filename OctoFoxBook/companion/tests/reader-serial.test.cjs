const test=require('node:test'),assert=require('node:assert/strict');
const {ReaderSerial}=require('../src/reader-serial.js');
function port(fail=false){
 let controller;const commands=[];
 return {commands,async open(){},async close(){},async setSignals(){},
  readable:new ReadableStream({start(c){controller=c;}}),
  writable:new WritableStream({write(bytes){
   const command=new TextDecoder().decode(bytes).trim();commands.push(command);let response;
   if(command==='SYNC STATUS')response='SYNC STATUS device=reader-001122334455 busy=false';
   else if(command==='PROVISION PAIR BEGIN')response='PROVISION PAIR READY device=reader-001122334455';
   else if(command.startsWith('PROVISION FIELD '))response='OK PROVISION FIELD name='+command.split(' ')[2]+' accepted=true';
   else if(command==='PROVISION COMMIT')response=fail?'ERROR PROVISION reason=commit-verification-failed':'PROVISION COMMIT COMPLETE configured=true';
   else if(command==='PROVISION ABORT')response='OK PROVISION ABORT staged=false';
   else if(command==='SYNC RESUME')response='SYNC RESUMED';
   else throw Error('Unexpected command');
   const wire=new TextEncoder().encode('background diagnostic\n'+response+'\n');
   controller.enqueue(wire.slice(0,12));controller.enqueue(wire.slice(12));
  }})};
}
const config={device:'reader-001122334455',url:'https://books.example/reader-api/device',username:'device-test',key:'separate-secret'};
test('USB pairing preserves Wi-Fi and checks device identity before sending a key',async()=>{
 const stub=port(),reader=new ReaderSerial(stub);await reader.open();
 try{
  assert.equal(await reader.identify(),config.device);
  await assert.rejects(reader.provision({...config,device:'reader-ffffffffffff'}),/changed/);
  assert(!stub.commands.some(c=>c.startsWith('PROVISION FIELD')));
  assert.equal(stub.commands.at(-1),'PROVISION ABORT');
  await reader.provision(config);
  assert(!stub.commands.some(c=>/FIELD (SSID|WIFI_PASSWORD)/.test(c)));
  assert(stub.commands.includes('PROVISION COMMIT'));assert.equal(stub.commands.at(-1),'SYNC RESUME');
 }finally{await reader.close();}
});
test('failed atomic save aborts staging without reporting successful pairing',async()=>{
 const stub=port(true),reader=new ReaderSerial(stub);await reader.open();
 try{await assert.rejects(reader.provision(config),/could not save/);assert.equal(stub.commands.at(-1),'PROVISION ABORT');}
 finally{await reader.close();}
});
