'use strict';
const $=id=>document.getElementById(id);
async function invoke(fn){$('error').hidden=true;try{const result=await fn();if(result?.error)showError(result.error);}catch{showError('Could not connect. Search again to retry.');}}
function showError(message){$('error').textContent=message;$('error').hidden=false;$('retry').hidden=false;$('progress').hidden=true;}
window.companion.status(data=>{
 document.body.classList.toggle('attached',data.phase==='connected');$('connected').hidden=data.phase!=='connected';
 $('servers').replaceChildren();$('error').hidden=true;$('retry').hidden=['scanning','connecting','connected'].includes(data.phase);$('progress').hidden=!['scanning','connecting'].includes(data.phase);
 $('address-form').querySelector('button').disabled=['scanning','connecting'].includes(data.phase);
 const labels={scanning:['Finding your library','Looking on this computer and your local network…'],connecting:['Opening your library',data.server?.name||'Connecting…'],empty:['No library found yet','Make sure OctoFox Server is running on this computer or on the same network. If your network blocks discovery, you can enter its address below.'],choose:['Choose your library','We found these libraries. Which one would you like to open?'],error:['Let’s reconnect',data.message]};
 const [title,description]=labels[data.phase]||['Your library is ready',''];$('title').textContent=title;$('description').textContent=description;
 if(data.server)$('server-name').textContent=data.server.name;
 for(const server of data.servers||[]){const button=document.createElement('button'),name=document.createElement('strong'),address=document.createElement('small');name.textContent=server.name;address.textContent=server.origin+(server.configured?'':' · Set up your library');button.append(name,address);button.onclick=()=>invoke(()=>window.companion.connect(server.id));$('servers').append(button);}
});
$('retry').onclick=()=>invoke(()=>window.companion.scan());$('switch').onclick=()=>invoke(()=>window.companion.disconnect());
$('manage').onclick=()=>invoke(()=>window.companion.navigate('companion'));$('library').onclick=()=>invoke(()=>window.companion.navigate('library'));
$('address-form').onsubmit=event=>{event.preventDefault();invoke(()=>window.companion.manual($('address').value.trim()));};
invoke(()=>window.companion.scan());
