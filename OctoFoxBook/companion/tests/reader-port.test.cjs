const test=require('node:test'),assert=require('node:assert/strict');
const {selectReaderPort}=require('../src/reader-port.cjs');
const reader={portId:'reader-com3',portName:'COM3',vendorId:'12346',productId:'4097'};
test('physical Windows Electron enumeration selects the T5 USB port',()=>{
 assert.equal(selectReaderPort([reader,{portId:'unrelated',vendorId:'9025',productId:'67'}]),reader.portId);
});
test('missing identifiers, malformed IDs and two readers are never guessed',()=>{
 for(const ports of [[],[{portId:'unknown'}],[{...reader,vendorId:'12346extra'}],[{...reader,productId:'4098'}],[reader,{...reader,portId:'second'}]])assert.equal(selectReaderPort(ports),'');
});
