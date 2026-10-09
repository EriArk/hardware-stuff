'use strict';
const $=id=>document.getElementById(id);
let token=location.hash.slice(1)||sessionStorage.getItem('octofox-installer');
if(token)sessionStorage.setItem('octofox-installer',token);
history.replaceState(null,'','/');
let polling=false,closed=false;
async function request(path,body){
 const response=await fetch('/api/'+path,{method:body?'POST':'GET',headers:{Authorization:'Bearer '+token,...(body?{'Content-Type':'application/json'}:{})},...(body?{body:JSON.stringify(body)}:{})});
 if(!response.ok)throw Error(response.status===403?'This installer session has expired. Reopen the server installer.':'The request could not complete. Wait for the current operation, then try again.');
 return response.json();
}
function error(message){$('error').textContent=message;$('error').hidden=false;}
function busy(value){for(const input of $('form').querySelectorAll('input,button'))input.disabled=value;$('progress').hidden=!value;}
function values(){return{directory:$('directory').value,speech:$('speech').checked,port:Number($('port').value),adminPort:Number($('adminPort').value)};}
function render(state){
 $('status').hidden=false;$('phase').textContent=state.phase;$('detail').textContent=state.detail;busy(state.busy);
 $('error').hidden=!state.error;if(state.error)error(state.error);
 if(state.result?.url){$('form').hidden=true;$('status').hidden=true;$('success').hidden=false;$('installed-path').textContent=state.result.directory;}
}
async function poll(){if(polling||closed)return;polling=true;try{render(await request('state'));}catch(e){error('The installer is no longer responding. Reopen it to check or retry; your server data is kept.');busy(false);}finally{polling=false;if(!closed)setTimeout(poll,1000);}}
async function action(kind){if(!$('form').reportValidity())return;$('error').hidden=true;busy(true);try{await request(kind,values());}catch(e){error(e.message);busy(false);}}
$('form').addEventListener('submit',e=>{e.preventDefault();action('install');});
$('check').addEventListener('click',()=>action('check'));
$('finish').addEventListener('click',async()=>{try{await request('close',{});closed=true;sessionStorage.removeItem('octofox-installer');$('finish').disabled=true;$('finish').textContent='All done';$('success').querySelector('p:not(.eyebrow)').textContent='You can close this tab. Your book server stays running.';}catch(e){error(e.message);}});
(async()=>{try{const defaults=await request('defaults');for(const id of ['directory','port','adminPort'])$(id).value=defaults[id];$('speech').checked=defaults.speech;poll();}catch(e){error(e.message);busy(true);$('progress').hidden=true;}})();
