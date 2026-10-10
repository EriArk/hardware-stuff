/* Local shell only. The hosted library never receives a serial capability. */
(function(root) {
  'use strict';
  class ReaderSerial {
    constructor(port) { this.port=port; this.pending=null; this.buffer=''; }
    async open() {
      await this.port.open({baudRate:115200});
      await this.port.setSignals({dataTerminalReady:true,requestToSend:false});
      this.reader=this.port.readable.getReader(); this.writer=this.port.writable.getWriter();
      this.pump=this.readLoop();
    }
    async readLoop() {
      const decoder=new TextDecoder();
      try {
        for(;;) {
          const {value,done}=await this.reader.read();if(done)break;
          this.buffer+=decoder.decode(value,{stream:true});
          if(this.buffer.length>65536)this.buffer=this.buffer.slice(-8192);
          let end;
          while((end=this.buffer.indexOf('\n'))>=0) {
            const line=this.buffer.slice(0,end).trim();this.buffer=this.buffer.slice(end+1);
            const pending=this.pending;if(!pending)continue;
            if(line.startsWith('ERROR ')) {this.pending=null;pending.reject(new Error('The reader could not save the connection. Your previous configuration is kept.'));}
            else if(line.startsWith(pending.expected)) {this.pending=null;pending.resolve(line);}
          }
        }
      } catch { if(this.pending)this.pending.reject(new Error('USB disconnected. Reconnect the reader and try again.')); }
      finally { this.reader.releaseLock(); }
    }
    async command(command,expected) {
      if(this.pending)throw new Error('Another USB operation is still running.');
      let timer;
      const response=new Promise((resolve,reject)=>{
        this.pending={expected,resolve,reject};
        timer=setTimeout(()=>{this.pending=null;reject(new Error('The reader did not respond. Reconnect it and try again.'));},15000);
      });
      try {await this.writer.write(new TextEncoder().encode(command+'\n'));return await response;}
      finally {clearTimeout(timer);this.pending=null;}
    }
    async identify() {
      const line=await this.command('SYNC STATUS','SYNC STATUS ');
      const id=/device=(reader-[a-f0-9]{12})\b/.exec(line)?.[1];
      if(!id)throw new Error('Update the reader firmware before pairing.');
      if(/busy=true/.test(line))throw new Error('Wait for synchronization to finish on the reader.');
      return id;
    }
    async provision(config) {
      const line=await this.command('PROVISION PAIR BEGIN','PROVISION PAIR READY ');
      const fields={OPDS_URL:config.url,OPDS_USERNAME:config.username,OPDS_PASSWORD:config.key};
      try {
        if(!line.includes('device='+config.device))throw new Error('The connected reader changed. Pairing was cancelled.');
        for(const [name,value] of Object.entries(fields)) {
          const bytes=new TextEncoder().encode(value);
          const encoded=btoa(String.fromCharCode(...bytes));
          await this.command(`PROVISION FIELD ${name} ${encoded}`,`OK PROVISION FIELD name=${name} accepted=true`);
        }
        await this.command('PROVISION COMMIT','PROVISION COMMIT COMPLETE configured=true');
        await this.command('SYNC RESUME','SYNC RESUMED');
      } catch(error) {
        try {await this.command('PROVISION ABORT','OK PROVISION ABORT');}catch{}
        throw error;
      }
    }
    async close() {
      try {await this.reader?.cancel();await this.pump;}catch{}
      try {this.writer?.releaseLock();await this.port.close();}catch{}
    }
  }
  if(typeof module!=='undefined')module.exports={ReaderSerial};else root.ReaderSerial=ReaderSerial;
})(globalThis);
