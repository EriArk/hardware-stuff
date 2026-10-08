const {test} = require('node:test');
const assert = require('node:assert/strict');
const {PageVoice} = require('../src/octofox_library/web/page-voice.js');
function fixture(native=true, count=3) {
  let now=1000, id=0, chosen=0;
  const saved=[], followed=[], calls=[], timers=new Map(), entries=new Map(), states=[], finished=[];
  const pages=Array.from({length:count},(_,i)=>({book:'42',chapter:Math.floor(i/2),page:i%2,
    anchor:{block:0,char:i%2*100},end:{block:0,char:(i%2+1)*100},layout:'screen'}));
  class Audio {
    constructor(){this.currentTime=0;this.duration=20;this.playCount=0;this.seeking=false;this.ended=false;this.paused=true;}
    set src(value){this._src=value;this.currentTime=0;this.ended=false;this.paused=true;}
    get src(){return this._src;}
    get currentSrc(){return this._src || '';}
    canPlayType(){return native?'probably':'';}
    play(){this.playCount++;this.paused=false;return Promise.resolve();}
    pause(){this.paused=true;this.onpause?.();} load(){this.currentTime=0;this.ended=false;} removeAttribute(){}
  }
  const env={Audio,Date:{now:()=>now},crypto:{randomUUID:()=>`id-${++id}`},navigator:{audioSession:{type:'auto'}},
    setTimeout(fn,delay){timers.set(++id,{fn,delay});return id;},clearTimeout(id){timers.delete(id);}};
  const api=async(url,data)=>{
    calls.push([url,data]);
    if(url.endsWith('/speech')){
      entries.set(data.id,{state:'ready',complete:true,segments:[{index:0,at:0,duration:20,chapter:data.chapter,
        cues:[{start:0,anchor:data.anchor},{start:10,anchor:{...data.anchor,char:data.anchor.char+50}}]}]});
      return {};
    }
    if(url.endsWith('/status'))return structuredClone(entries.get(url.split('/')[3]));
    return {};
  };
  const voice=new PageVoice(api,{page:()=>pages[chosen],position:()=>({book:'42',chapter:pages[chosen].chapter,anchor:pages[chosen].anchor}),
    next:p=>pages[pages.findIndex(item=>item.chapter===p.chapter&&item.page===p.page)+1]||null,layout:()=> 'screen',
    state:(s,e)=>states.push([s,e]),save:p=>saved.push(structuredClone(p)),
    follow:(p,current)=>{if(current()){followed.push(structuredClone(p));return true;}return false;},
    finish:p=>finished.push(p)}, {available:true,voice:'eugene'},env);
  const tick=async()=>{await new Promise(setImmediate);await new Promise(setImmediate);};
  return {voice,api,env,calls,entries,pages,saved,followed,states,finished,timers,tick,
    flush:async delay=>{now+=delay;for(const [key,timer] of [...timers])if(timer.delay===delay){timers.delete(key);timer.fn();}await tick();},
    choose:i=>chosen=i,advance:(s,emit=true)=>{now+=s*1000;voice.audio.currentTime+=s*voice.backend.rate;if(emit)voice.audio.ontimeupdate?.();},
    started:async()=>{voice.play();await tick();voice.audio.onloadedmetadata();voice.audio.onplaying();await tick();},
    ended:async()=>{
      now+=25000;const audio=voice.audio;audio.currentTime=audio.duration;audio.ended=true;
      // Native media reaches the end: timeupdate, automatic pause, then ended.
      audio.ontimeupdate?.();audio.paused=true;audio.onpause?.();audio.onended?.();await tick();
    }};
}
function audioCache(f) {
  const fetched=[], created=[], revoked=[];
  f.env.AbortController=AbortController;
  f.env.URL={createObjectURL:blob=>{const url=`blob:page-${created.length}`;created.push([url,blob]);return url;},
    revokeObjectURL:url=>revoked.push(url)};
  f.env.fetch=async(url,options)=>{
    fetched.push([url,options]);
    return {ok:true,headers:{get:()=> '48000'},blob:async()=>({size:48000})};
  };
  return {fetched,created,revoked};
}

test('late server voice catalog becomes selectable without interrupting current audio',async()=>{
  const f=fixture();await f.started();
  const audio=f.voice.audio,epoch=f.voice.epoch;
  f.voice.updateInfo({available:true,voice:'ljspeech',voices:[
    {voiceURI:'eugene',lang:'ru-RU'}, {voiceURI:'ljspeech',lang:'en-US'}, {voiceURI:'siwis',lang:'fr-FR'}]});
  assert.deepEqual(f.voice.backend.voices().map(v=>v.voiceURI),['eugene','ljspeech','siwis']);
  assert.equal(f.voice.audio,audio);assert.equal(f.voice.epoch,epoch);
  assert.equal(f.voice.backend.voiceURI,'eugene');assert.equal(f.voice.status,'playing');
  f.voice.stop();f.voice.setVoice('ljspeech');f.voice.play();await f.tick();
  assert.equal(f.calls.filter(([url])=>url.endsWith('/speech')).at(-1)[1].voice,'ljspeech');
  f.voice.stop();
});
test('lookahead refills across chapters but never grows beyond current plus three pages',async()=>{
  const f=fixture(true,10);await f.started();
  for(let i=0;i<6;i++){
    assert.equal(f.voice.entries.size,4);
    assert.deepEqual([...f.voice.entries].map(e=>f.pages.indexOf(e.page)),[i,i+1,i+2,i+3]);
    await f.ended();f.voice.audio.onloadedmetadata();f.voice.audio.onplaying();await f.tick();
  }
  assert.equal(f.voice.current.page,f.pages[6]);f.voice.stop();assert.equal(f.voice.entries.size,0);
});
test('later pages do not compete with generation of the current page',async()=>{
  const f=fixture(true,8),api=f.voice.api;
  let ready=false;
  f.voice.api=(url,data)=>url.endsWith('/status')&&!ready?Promise.resolve({complete:false,segments:[]}):api(url,data);
  f.voice.play();await f.tick();assert.equal(f.voice.entries.size,1);
  ready=true;await f.voice.poll(f.voice.current,f.voice.epoch);await f.tick();
  assert.equal(f.voice.entries.size,4);f.voice.stop();
});
test('prepared future audio is downloaded once and played locally on the same element',async()=>{
  const f=fixture(true,8),cache=audioCache(f);await f.started();
  assert.equal(cache.fetched.length,3);assert.equal(cache.created.length,3);
  assert(cache.fetched.every(([,options])=>options.credentials==='same-origin'));
  const audio=f.voice.audio,next=f.voice.current.next,source=next.objectURL;
  await f.ended();assert.equal(f.voice.audio,audio);assert.equal(audio.src,source);
  audio.onloadedmetadata();audio.onplaying();await f.tick();
  assert.equal(f.followed.at(-1).anchor.char,100);assert.equal(f.voice.status,'playing');
  const old=audio.src;await f.ended();assert(cache.revoked.includes(old));
  f.voice.stop();assert.equal(cache.revoked.length,cache.created.length);
});
test('navigation aborts preload and a late response cannot resurrect cached audio',async()=>{
  const f=fixture(true,6),cache=audioCache(f);let resolve;
  f.env.fetch=(url,options)=>{cache.fetched.push([url,options]);return new Promise(r=>resolve=r);};
  await f.started();const pending=cache.fetched;
  f.voice.navigate();assert(pending.every(([,o])=>o.signal.aborted));
  resolve({ok:true,headers:{get:()=> '48000'},blob:async()=>({size:48000})});await f.tick();
  assert.equal(cache.created.length,0);assert.equal(f.voice.entries.size,0);f.voice.stop();
});
test('oversize preload is cancelled and next page uses normal URL playback',async()=>{
  const f=fixture(),cache=audioCache(f);let cancelled=0;
  f.env.fetch=async()=>({ok:true,headers:{get:()=>String(7*1024*1024)},body:{cancel:async()=>cancelled++},
    blob:async()=>{throw Error('oversize body must not be buffered');}});
  await f.started();assert.equal(cancelled,2);assert.equal(cache.created.length,0);
  await f.ended();assert(f.voice.audio.src.endsWith('/page.wav'));assert.equal(f.voice.wantsPlay,true);f.voice.stop();
});
test('failed preload is optional and cannot block automatic playback',async()=>{
  const f=fixture();audioCache(f);f.env.fetch=async()=>{throw Error('offline');};await f.started();
  await f.ended();assert.equal(f.voice.current.page,f.pages[1]);assert(f.voice.audio.src.endsWith('/page.wav'));f.voice.stop();
});
test('audio finishing download after activation must not replace a playing source',async()=>{
  const f=fixture();audioCache(f);const pending=[];
  f.env.fetch=async()=>({ok:true,headers:{get:()=> '48000'},blob:()=>new Promise(r=>pending.push(r))});
  await f.started();await f.ended();const source=f.voice.audio.src;
  pending.forEach(r=>r({size:48000}));await f.tick();
  assert.equal(f.voice.audio.src,source);assert.equal(f.voice.current.objectURL,undefined);f.voice.stop();
});
test('an unsupported cached source falls back to direct audio without changing page',async()=>{
  const f=fixture(),cache=audioCache(f);await f.started();await f.ended();
  const entry=f.voice.current,source=f.voice.audio.src;assert(source.startsWith('blob:'));
  f.voice.audio.onerror();assert(f.voice.audio.src.endsWith('/page.wav'));
  assert.equal(f.voice.current,entry);assert(cache.revoked.includes(source));assert.equal(f.voice.wantsPlay,true);f.voice.stop();
});
test('bounded current page plus three next pages are prepared with exact end anchors',async()=>{
  const f=fixture(true,8);await f.started();
  const requests=f.calls.filter(([u])=>u.endsWith('/speech'));
  assert.equal(requests.length,4);assert.equal(f.voice.entries.size,4);
  assert.equal(requests[0][1].end.char,100);assert.equal(requests[1][1].anchor.char,100);
  assert.equal(f.saved.at(-1).chapter,0);assert.equal(f.saved.at(-1).anchor.char,0);
  assert(f.voice.audio.src.endsWith('/page.wav'));f.voice.stop();
});
test('end automatically uses pre-generated next page and the same audio element, then crosses chapter',async()=>{
  const f=fixture();await f.started();const audio=f.voice.audio,oldTime=audio.ontimeupdate;
  await f.ended();assert.equal(f.voice.audio,audio);assert.equal(f.voice.current.page.page,1);
  audio.currentTime=0;audio.onloadedmetadata();audio.onplaying();await f.tick();
  assert.equal(f.followed.at(-1).anchor.char,100);
  oldTime();assert.equal(f.followed.at(-1).anchor.char,100);
  await f.ended();assert.equal(f.voice.current.page.chapter,1);
  audio.currentTime=0;audio.onloadedmetadata();audio.onplaying();await f.tick();
  assert.equal(f.followed.at(-1).chapter,1);assert.equal(f.finished.length,0);
  await f.ended();assert.equal(f.finished.length,1);assert.equal(f.voice.status,'ended');f.voice.stop();
});
test('unsolicited timeline jump cannot turn pages or overwrite bookmark',async()=>{
  const f=fixture();await f.started();f.advance(13);await f.tick();
  const before=structuredClone(f.saved.at(-1)),page=f.voice.current;
  f.voice.audio.currentTime=3000;f.voice.audio.ontimeupdate();await f.tick();
  assert.deepEqual(f.saved.at(-1),before);assert.equal(f.voice.current,page);assert(f.voice.audio.currentTime<20);f.voice.stop();
});
test('manual navigation cancels current and prefetch; late audio cannot reverse it',async()=>{
  const f=fixture();await f.started();const old=f.voice.audio,callback=old.ontimeupdate;
  assert.equal(f.voice.navigate(),true);f.choose(2);f.voice.play();await f.tick();
  const saved=f.saved.length;old.currentTime=10;callback();await f.tick();
  assert.equal(f.saved.length,saved);assert.equal(f.voice.current.page.chapter,1);
  f.voice.stop();await f.tick();assert.equal(f.voice.entries.size,0);
});
test('pause and resume retain page/audio/time, not a fresh page start',async()=>{
  const f=fixture();await f.started();f.advance(13);await f.tick();
  const audio=f.voice.audio,time=audio.currentTime,id=f.voice.current.id;
  f.voice.pause();f.voice.play();assert.equal(f.voice.audio,audio);assert.equal(audio.currentTime,time);assert.equal(f.voice.current.id,id);f.voice.stop();
});
test('paragraph cue follows while loading view retries; cancellation cannot apply late highlight',async()=>{
  const f=fixture();await f.started();let ready=false;
  f.voice.view.follow=()=>ready;
  f.advance(13);await f.tick();assert.equal(f.saved.at(-1).anchor.char,50);
  assert.notEqual(f.voice.applied,JSON.stringify(f.saved.at(-1)));ready=true;f.voice.follow();await f.tick();
  assert.equal(f.voice.applied,JSON.stringify(f.saved.at(-1)));f.voice.stop();
});
test('desktop fallback requests one complete page WAV, not independent chunks',async()=>{
  const f=fixture(false);await f.started();assert(f.voice.audio.src.endsWith('/page.wav'));f.voice.stop();
});
test('natural pause before ended continues WAV playback and saves the next page',async()=>{
  const f=fixture(false);await f.started();const audio=f.voice.audio;
  await f.ended();assert.equal(f.voice.wantsPlay,true);assert.equal(f.voice.current.page,f.pages[1]);
  assert.equal(audio.playCount,2);assert.equal(f.saved.at(-1).anchor.char,100);
  audio.onloadedmetadata();audio.onplaying();await f.tick();
  assert.equal(f.followed.at(-1).anchor.char,100);assert.equal(f.voice.status,'playing');f.voice.stop();
});
test('an actual native pause near the end still pauses instead of turning the page',async()=>{
  const f=fixture();await f.started();f.advance(23);await f.tick();
  const entry=f.voice.current;f.voice.audio.pause();
  await f.flush(200);
  assert.equal(f.voice.status,'paused');assert.equal(f.voice.wantsPlay,false);
  await f.ended();assert.equal(f.voice.current,entry);assert.equal(f.finished.length,0);f.voice.stop();
});
test('iPhone pause at the finite end advances even without ended flag or event',async()=>{
  const f=fixture();await f.started();f.voice.setRate(1.05);f.advance(19);await f.tick();
  const audio=f.voice.audio,oldEnded=audio.onended;audio.pause();
  assert.equal(audio.ended,false);assert.equal(f.voice.wantsPlay,true);
  await f.flush(200);
  assert.equal(f.voice.current.page,f.pages[1]);assert.equal(audio.playCount,2);
  assert.equal(f.saved.at(-1).anchor.char,100);
  audio.onloadedmetadata();audio.onplaying();await f.tick();oldEnded();await f.tick();
  assert.equal(f.voice.current.page,f.pages[1]);assert.equal(audio.playCount,2);f.voice.stop();
});
test('late ended is not needed but must never advance twice',async()=>{
  const f=fixture();await f.started();f.advance(20/.85);await f.tick();
  const audio=f.voice.audio,oldEnded=audio.onended;audio.pause();
  await f.flush(200);assert.equal(f.voice.current.page,f.pages[1]);
  oldEnded();await f.tick();assert.equal(audio.playCount,2);f.voice.stop();
});
test('explicit pause on the final audio frames cancels any pending automatic advance',async()=>{
  const f=fixture();await f.started();f.advance(19.95/.85);await f.tick();
  const entry=f.voice.current;f.voice.audio.pause();f.voice.pause();
  await f.flush(200);await f.ended();
  assert.equal(f.voice.current,entry);assert.equal(f.voice.status,'paused');f.voice.stop();
});
test('a system interruption at the end must not restart audio',async()=>{
  const f=fixture();await f.started();f.advance(19.95/.85);await f.tick();
  const entry=f.voice.current;f.env.navigator.audioSession.state='interrupted';f.voice.audio.pause();
  await f.flush(200);assert.equal(f.voice.current,entry);assert.equal(f.voice.wantsPlay,false);f.voice.stop();
});
test('pause after buffering mid-page cancels playback intent too',async()=>{
  const f=fixture();await f.started();f.advance(10);await f.tick();
  f.voice.audio.onwaiting();f.voice.audio.pause();await f.flush(200);
  assert.equal(f.voice.status,'paused');assert.equal(f.voice.wantsPlay,false);f.voice.stop();
});
test('pause while waiting for the next page is resumable without replaying the finished page',async()=>{
  const f=fixture();let resolve;f.voice.view.next=()=>new Promise(r=>resolve=r);
  await f.started();const entry=f.voice.current;await f.ended();assert.equal(f.voice.status,'loading');
  f.voice.pause();resolve(f.pages[1]);await f.tick();
  assert.equal(f.voice.current,entry);assert.equal(f.voice.status,'paused');
  f.voice.play();await f.tick();assert.equal(f.voice.current.page,f.pages[1]);assert.equal(f.voice.audio.playCount,2);f.voice.stop();
});
test('short or implausible media endings cannot skip unread text',async()=>{
  const f=fixture();await f.started();const entry=f.voice.current;
  f.advance(10);await f.tick();f.voice.audio.duration=8.5;f.voice.audio.pause();await f.flush(200);
  assert.equal(f.voice.current,entry);assert.equal(f.voice.status,'paused');f.voice.stop();
});
test('WAV cue and end timing use PCM durations, not padded HLS durations',async()=>{
  const f=fixture();await f.started();
  f.voice.current.info.segments=[
    {at:0,duration:10.3,pcmAt:0,pcmDuration:10,cues:[{start:0,anchor:{block:0,char:0}}]},
    {at:10.3,duration:10.3,pcmAt:10,pcmDuration:10,cues:[{start:0,anchor:{block:0,char:50}}]},
  ];
  f.advance(10.1/.85);await f.tick();assert.equal(f.saved.at(-1).anchor.char,50);
  await f.ended();assert.equal(f.voice.current.page,f.pages[1]);f.voice.stop();
});
test('failure preparing next page does not finish book or skip its text',async()=>{
  const f=fixture();await f.started();f.voice.current.next.error=Error('offline');
  await f.ended();assert.equal(f.voice.status,'error');assert.equal(f.finished.length,0);
});
test('late prepare after stop is cleaned up and cannot revive audio',async()=>{
  const f=fixture();let release;const api=f.voice.api;
  f.voice.api=(url,data)=>url.endsWith('/speech')?new Promise(r=>release=()=>{api(url,data).then(r);}):api(url,data);
  f.voice.play();f.voice.stop();release();await f.tick();assert.equal(f.voice.audio,null);assert.equal(f.voice.entries.size,0);
});
test('persistent status failure exits preparation with a retryable error',async()=>{
  const f=fixture();const api=f.voice.api;
  f.voice.api=(u,d)=>u.endsWith('/status')?Promise.reject(Error('offline')):api(u,d);
  f.voice.play();await f.tick();const entry=f.voice.current;
  await f.voice.poll(entry,f.voice.epoch);await f.voice.poll(entry,f.voice.epoch);
  assert.equal(f.voice.status,'error');assert.equal(f.voice.audio,null);
});

test('two listeners: delayed completion status cannot turn a natural page end into a user pause',async()=>{
  const a=fixture(true,6), b=fixture(true,6);
  let ready=false;
  const api=b.voice.api;
  b.voice.api=(url,data)=>url.endsWith('/status')&&!ready
    ? Promise.resolve({state:'preparing',complete:false,segments:[]}) : api(url,data);
  await Promise.all([a.started(),b.started()]);
  const waiting=b.voice.current;
  // The finite file is ready before the independent status request catches up.
  b.advance(20/.85);b.voice.audio.pause();
  await Promise.all([a.ended(),b.flush(200)]);
  assert.equal(a.voice.current.page,a.pages[1]);
  assert.equal(b.voice.wantsPlay,true,'Waiting for metadata is not a user pause');
  assert.equal(b.voice.current,waiting,'Do not skip before validating the complete PCM duration');
  ready=true;await b.voice.poll(waiting,b.voice.epoch);await b.tick();
  assert.equal(b.voice.current.page,b.pages[1]);
  assert.equal(b.voice.wantsPlay,true);
  a.voice.stop();b.voice.stop();
});

test('late readiness after an explicit pause never restarts or advances audio',async()=>{
  const f=fixture(),api=f.voice.api;let ready=false;
  f.voice.api=(url,data)=>url.endsWith('/status')&&!ready
    ? Promise.resolve({state:'preparing',complete:false,segments:[]}) : api(url,data);
  await f.started();const entry=f.voice.current;
  f.advance(20/.85);f.voice.audio.pause();await f.flush(200);
  f.voice.pause();ready=true;await f.voice.poll(entry,f.voice.epoch);await f.tick();
  assert.equal(f.voice.wantsPlay,false);assert.equal(f.voice.status,'paused');
  assert.equal(f.voice.current,entry);f.voice.stop();
});

test('delayed metadata must still reject a truncated end and keep the same page',async()=>{
  const f=fixture(),api=f.voice.api;let ready=false;
  f.voice.api=(url,data)=>url.endsWith('/status')&&!ready
    ? Promise.resolve({state:'preparing',complete:false,segments:[]}) : api(url,data);
  await f.started();const entry=f.voice.current;
  f.voice.audio.duration=10;f.advance(10/.85);f.voice.audio.pause();await f.flush(200);
  assert.equal(f.voice.wantsPlay,true);assert.equal(f.voice.status,'loading');
  ready=true;await f.voice.poll(entry,f.voice.epoch);await f.tick();
  assert.equal(f.voice.current,entry);assert.equal(f.voice.status,'paused');f.voice.stop();
});

test('live speed reduction rebases the old-rate clock without seeking or replacing cached audio',async()=>{
  const f=fixture();await f.started();
  const audio=f.voice.audio,entry=f.voice.current,requests=f.calls.length;
  f.advance(18,false);const time=audio.currentTime;
  f.voice.setRate(.5);await f.tick();
  assert.equal(audio.currentTime,time);assert.equal(f.voice.lastTime,time);
  assert.equal(audio.playbackRate,.5);assert.equal(f.voice.current,entry);
  assert.equal(f.calls.length,requests);assert.equal(audio.playCount,1);
  f.advance(2);assert.equal(audio.currentTime,time+1);
  f.voice.pause();f.voice.setRate(1.2);assert.equal(f.voice.status,'paused');
  f.voice.play();assert.equal(audio.playbackRate,1.2);f.voice.stop();
});

test('changing voice during playback resumes the spoken phrase, not the top of the page',async()=>{
  const f=fixture(),cache=audioCache(f);await f.started();f.advance(13);await f.tick();
  const old=f.voice.audio,late=old.ontimeupdate;
  f.voice.setVoice('kseniya');await f.tick();
  const fresh=f.voice.current;
  assert.equal(f.voice.audio,old,'Keep the already-authorized media element');
  assert.equal(fresh.page.anchor.char,50);assert.equal(fresh.page.end.char,100);
  assert(f.calls.filter(([u])=>u.endsWith('/speech')).slice(-3).every(([,p])=>p.voice==='kseniya'));
  assert.equal(cache.revoked.length,2,'Old lookahead audio is discarded');
  assert.equal(f.voice.wantsPlay,true);
  const saved=f.saved.length;late();assert.equal(f.saved.length,saved,'Old voice cannot move the bookmark');
  f.voice.audio.onloadedmetadata();f.voice.audio.onplaying();await f.tick();
  await f.ended();assert.equal(f.voice.current.page,f.pages[1]);f.voice.stop();
});

test('changing voice during next-page loading preserves the next page and play intent',async()=>{
  const f=fixture();await f.started();await f.ended();
  assert.equal(f.voice.status,'loading');assert.equal(f.voice.current.page,f.pages[1]);
  f.voice.setVoice('ruslan');await f.tick();
  assert.equal(f.voice.current.page.chapter,0);assert.equal(f.voice.current.page.anchor.char,100);
  assert.equal(f.voice.voice,'ruslan');assert.equal(f.voice.wantsPlay,true);f.voice.stop();
});

test('voice change while paused waits for play, and manual navigation clears its resume anchor',async()=>{
  const f=fixture();await f.started();f.advance(13);await f.tick();f.voice.pause();
  f.voice.setVoice('ruslan');await f.tick();
  assert.equal(f.voice.status,'paused');assert.equal(f.voice.audio,null);
  f.voice.play();await f.tick();assert.equal(f.voice.current.page.anchor.char,50);
  f.voice.audio.onloadedmetadata();f.voice.audio.onplaying();await f.tick();f.voice.pause();
  f.voice.setVoice('kseniya');f.voice.navigate();f.choose(2);f.voice.play();await f.tick();
  assert.equal(f.voice.current.page,f.pages[2]);f.voice.stop();
});

test('font reflow resumes within the newly measured page at the spoken anchor',async()=>{
  const f=fixture();await f.started();f.advance(13);await f.tick();
  const anchor=structuredClone(f.voice.spokenPosition.anchor),old=f.voice.current,audio=f.voice.audio;
  f.voice.view.layout=()=> 'large-font';
  f.voice.view.page=position=>{
    assert.deepEqual(position.anchor,anchor);
    return {...f.pages[0],anchor:{block:0,char:40},end:{block:0,char:80},layout:'large-font'};
  };
  f.voice.reflow();f.voice.reflow();f.voice.reflow();await f.tick();
  assert.equal(f.voice.current,old,'Wait until the font slider settles');
  await f.flush(200);
  assert.notEqual(f.voice.current,old);assert.equal(f.voice.wantsPlay,true);
  assert.equal(f.voice.audio,audio,'Reflow must reuse the media element unlocked on iPhone');
  assert.equal(f.voice.current.page.anchor.char,50);assert.equal(f.voice.current.page.end.char,80);
  assert.equal(f.voice.current.page.layout,'large-font');
  const entry=f.voice.current;f.voice.reflow();assert.equal(f.voice.current,entry,'No restart for unchanged geometry/theme');
  f.voice.stop();
});

test('font reflow while paused never resumes until requested',async()=>{
  const f=fixture();await f.started();f.advance(13);await f.tick();f.voice.pause();
  f.voice.view.layout=()=> 'new-size';
  f.voice.view.page=()=>({...f.pages[0],layout:'new-size'});
  f.voice.reflow();await f.flush(200);assert.equal(f.voice.status,'paused');assert.equal(f.voice.audio,null);
  f.voice.play();await f.tick();assert.equal(f.voice.current.page.anchor.char,50);f.voice.stop();
});

test('changing one listener settings does not affect the other on the same book',async()=>{
  const a=fixture(),b=fixture();await Promise.all([a.started(),b.started()]);
  b.advance(13);await b.tick();const entry=b.voice.current,audio=b.voice.audio,time=audio.currentTime;
  a.voice.setVoice('kseniya');a.voice.setRate(.5);await a.tick();
  assert.equal(b.voice.current,entry);assert.equal(b.voice.audio,audio);assert.equal(audio.currentTime,time);
  assert.equal(b.voice.voice,'eugene');assert.equal(b.voice.backend.rate,.85);
  await b.ended();assert.equal(b.voice.current.page,b.pages[1]);a.voice.stop();b.voice.stop();
});

test('stopping while a font change is pending cancels its delayed restart',async()=>{
  const f=fixture();await f.started();f.voice.view.layout=()=> 'new-size';
  f.voice.reflow();f.voice.stop();await f.flush(200);
  assert.equal(f.voice.audio,null);assert.equal(f.voice.status,'idle');assert.equal(f.voice.entries.size,0);
});
