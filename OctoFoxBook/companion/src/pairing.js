'use strict';
let readerPort,readerDevice,pairingBusy=false,currentPairAccount='';
const pairNode=id=>document.getElementById(id);
function pairMessage(text){pairNode('pair-status').textContent=text;}
async function pairInfo() {
  const info=await window.companion.reader('info');
  if(info.error)throw Error(info.error);
  currentPairAccount=info.currentAccount||'';
  if(!pairNode('pair-account').value)pairNode('pair-account').value=currentPairAccount;
  pairAccountChanged();
  pairNode('pair-origin').replaceChildren();
  for(const origin of info.origins.filter(o=>o.startsWith('https://'))) {
    const option=document.createElement('option');option.value=origin;option.textContent=origin;pairNode('pair-origin').append(option);
  }
  pairNode('pair-keys').replaceChildren();
  for(const reader of info.readers.filter(r=>!r.revoked)) {
    const row=document.createElement('p'),button=document.createElement('button');
    row.textContent=reader.owner+' · '+reader.device+' ';button.textContent='Revoke access';
    button.onclick=async()=>{
      button.disabled=true;
      const result=await window.companion.reader('revoke',{username:reader.username});
      if(result.error){pairMessage(result.error);button.disabled=false;}else await pairInfo();
    };
    row.append(button);pairNode('pair-keys').append(row);
  }
  pairNode('pair-submit').disabled=!readerDevice||!pairNode('pair-origin').value;
  return info;
}
function pairAccountChanged(){
  const needsPassword=pairNode('pair-account').value!==currentPairAccount;
  pairNode('pair-password').required=needsPassword;
  pairNode('pair-password-row').hidden=!needsPassword;
  if(!needsPassword)pairNode('pair-password').value='';
}
pairNode('pair-account').oninput=pairAccountChanged;
pairNode('pair-reader').onclick=async()=>{
  await window.companion.pairPanel(true);pairNode('pair-panel').hidden=false;
  pairMessage('Connect your AbyssBook by USB, then choose Connect reader.');
  try {await pairInfo();if(!pairNode('pair-origin').value)pairMessage('Add your public HTTPS address in Companion → Network first.');}
  catch(error){pairMessage(error.message);}
};
pairNode('pair-close').onclick=async()=>{
  if(pairingBusy)return;
  await readerPort?.close();readerPort=null;readerDevice=null;
  pairNode('pair-password').value='';pairNode('pair-panel').hidden=true;await window.companion.pairPanel(false);
};
pairNode('pair-usb').onclick=async()=>{
  try {
    await readerPort?.close();readerPort=null;readerDevice=null;
    const port=await navigator.serial.requestPort({filters:[{usbVendorId:0x303a,usbProductId:0x1001}]});
    readerPort=new ReaderSerial(port);await readerPort.open();readerDevice=await readerPort.identify();
    pairMessage('Connected: '+readerDevice+'. Choose the library account below.');
    pairNode('pair-submit').disabled=!pairNode('pair-origin').value;
  }catch(error){pairMessage(error.message);pairNode('pair-submit').disabled=true;}
};
pairNode('pair-form').onsubmit=async event=>{
  event.preventDefault();if(pairingBusy||!readerDevice)return;
  pairingBusy=true;for(const id of ['pair-submit','pair-close','pair-usb'])pairNode(id).disabled=true;
  let config;
  try {
    pairMessage('Creating a separate key for this reader…');
    config=await window.companion.reader('create',{device:readerDevice,username:pairNode('pair-account').value,
      password:pairNode('pair-password').value,origin:pairNode('pair-origin').value});
    pairNode('pair-password').value='';
    if(config.error)throw Error(config.error);
    pairMessage('Saving the connection to the reader…');await readerPort.provision(config);
    pairMessage('Paired. On the reader, choose Wi-Fi in Settings, then Library → Synchronize. It will also work away from home.');
    await pairInfo();
  }catch(error){
    // A lost USB acknowledgement may follow a successful atomic save. Keep the
    // new key visible in the list so it can be retried or explicitly revoked.
    pairMessage(error.message+' If a key was created, it is listed below.');
    try{await pairInfo();}catch{}
  }finally{
    if(config)config.key='';pairNode('pair-password').value='';pairingBusy=false;
    for(const id of ['pair-submit','pair-close','pair-usb'])pairNode(id).disabled=false;
    pairNode('pair-submit').disabled=!readerDevice||!pairNode('pair-origin').value;
  }
};
