const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
const {execFileSync}=require('child_process');
const ip=process.argv[2]||'127.0.0.1', adb=process.env.ADB;
function command(...args){return execFileSync(adb,['-s',process.env.ADB_SERIAL,...args],{encoding:'utf8'});}
async function tap(label){
  command('shell','uiautomator','dump','/sdcard/lanscreencast-ui.xml');
  const xml=command('shell','cat','/sdcard/lanscreencast-ui.xml');
  const m=xml.match(new RegExp('text="'+label+'"[^>]*bounds="\\[(\\d+),(\\d+)\\]\\[(\\d+),(\\d+)\\]"'));
  if(!m)throw Error('Receiver UI button missing: '+label);
  command('shell','input','tap',String((+m[1]+ +m[3])/2),String((+m[2]+ +m[4])/2));
}
(async()=>{
  if(!adb)throw Error('Set ADB and ADB_SERIAL for receiver interaction');
  const browser=await chromium.launch({headless:true,executablePath:process.env.CHROME||undefined,
    args:['--disable-features=LocalNetworkAccessChecks,PrivateNetworkAccessSendPreflights']});
  try{
    const page=await browser.newPage();await page.goto('http://127.0.0.1:8765/');
    await page.evaluate(ip=>{
      window.probes={};
      window.startProbe=(name,version=2,heartbeat=true,badOrder=false)=>{
        const sessionId=crypto.randomUUID(),ws=new WebSocket('ws://'+ip+':47475/signaling');
        const probe=window.probes[name]={closed:false,messages:[],started:performance.now()};
        const send=(type,payload={})=>ws.send(JSON.stringify({protocolVersion:version,type,requestId:crypto.randomUUID(),timestamp:Date.now(),payload:{sessionId,...payload}}));
        ws.onopen=()=>{send('hello',{computerName:name});if(badOrder)send('offer',{sdp:'v=0',video:{width:1280,height:720,fps:30,bitrate:4000000}})};
        ws.onmessage=e=>{const m=JSON.parse(e.data);probe.messages.push(m);if(m.type==='capabilities')probe.acceptedAt=performance.now();if(heartbeat&&m.type==='ping')send('pong')};
        const timer=setInterval(()=>{if(heartbeat&&ws.readyState===1)send('ping')},2000);
        ws.onclose=()=>{probe.closed=true;probe.elapsed=performance.now()-probe.started;probe.mediaElapsed=performance.now()-probe.acceptedAt;clearInterval(timer)};
      };
    },ip);
    const start=(...args)=>page.evaluate(args=>window.startProbe(...args),args);
    const response=async(name,code)=>{
      await page.waitForFunction(({name,code})=>window.probes[name].messages.some(x=>x.type==='error'&&x.payload.code===code),{name,code},{timeout:12000});
      console.log(name,code);
    };
    const closed=async(name)=>{
      await page.waitForFunction(name=>window.probes[name].closed,name,{timeout:15000});
      return page.evaluate(name=>window.probes[name].elapsed,name);
    };
    await start('version',1);await response('version','VERSION_MISMATCH');await closed('version');
    await start('reject');await new Promise(r=>setTimeout(r,800));await tap('拒绝');
    await response('reject','REJECTED');await closed('reject');
    await start('pending');await new Promise(r=>setTimeout(r,800));await start('busy');
    await response('busy','BUSY');await closed('busy');await tap('拒绝');await response('pending','REJECTED');await closed('pending');
    await start('badOrder',2,true,true);await response('badOrder','INVALID_MESSAGE');await closed('badOrder');
    await start('silent',2,false);const silence=await closed('silent');
    if(silence<5000||silence>10000)throw Error('Heartbeat deadline mismatch: '+silence);
    console.log('Heartbeat timeout',silence);
    await start('mediaTimeout');await new Promise(r=>setTimeout(r,800));await tap('接受');
    await closed('mediaTimeout');
    const media=await page.evaluate(()=>window.probes.mediaTimeout.mediaElapsed);
    if(media<9000||media>13000)throw Error('Media deadline mismatch: '+media);
    console.log('Media timeout',media);
    console.log('Version, rejection, pending BUSY, invalid order, heartbeat and media deadlines verified');
  }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1});
