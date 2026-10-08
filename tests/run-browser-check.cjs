const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
const {execFileSync}=require('child_process');
const ip=process.argv[2]||'127.0.0.1';
const width=+(process.argv[3]||1280);
const seconds=+(process.argv[4]||12);
const adb=process.env.ADB;
function command(...args){return execFileSync(adb,['-s',process.env.ADB_SERIAL,...args],{encoding:'utf8'});}
async function tap(label){
  if(!adb)return;
  command('shell','uiautomator','dump','/sdcard/lanscreencast-ui.xml');
  const xml=command('shell','cat','/sdcard/lanscreencast-ui.xml');
  const match=xml.match(new RegExp('text="'+label+'"[^>]*bounds="\\[(\\d+),(\\d+)\\]\\[(\\d+),(\\d+)\\]"'));
  if(match)command('shell','input','tap',String((+match[1]+ +match[3])/2),String((+match[2]+ +match[4])/2));
}
(async()=>{
  const browser=await chromium.launch({headless:true,
    executablePath:process.env.CHROME||undefined,
    args:['--disable-features=LocalNetworkAccessChecks,PrivateNetworkAccessSendPreflights',
      '--disable-background-timer-throttling','--disable-renderer-backgrounding','--disable-backgrounding-occluded-windows']});
  try{
    const page=await browser.newPage();
    page.on('pageerror',e=>console.error('PAGE ERROR',e.message));
    await page.goto('http://127.0.0.1:8765/tests/webrtc-browser.html?ip='+ip+'&width='+width);
    console.log('Awaiting receiver acceptance');
    await new Promise(r=>setTimeout(r,1500));
    await tap('接受');
    await page.waitForFunction(()=>window.results.connected,null,{timeout:20000});
    await new Promise(r=>setTimeout(r,seconds*1000));
    const result=await page.evaluate(async()=>{
      const stats=await [...(await pc.getStats()).values()];
      return {receiver:window.results,codecs:stats.filter(x=>x.type==='codec'),
        outbound:stats.filter(x=>x.type==='outbound-rtp'),connection:pc.connectionState};
    });
    if(adb && process.env.SCREENSHOT)require('fs').writeFileSync(process.env.SCREENSHOT,
      execFileSync(adb,['-s',process.env.ADB_SERIAL,'exec-out','screencap','-p']));
    console.log(JSON.stringify(result,null,2));
    if(!result.receiver.stats.some(x=>x.fps>0))throw Error('Receiver reported no rendered video');
    if(!result.codecs.some(x=>x.mimeType==='video/H264'))throw Error('Not H264');
    const busy=await page.evaluate(()=>new Promise((resolve,reject)=>{
      const second=new WebSocket('ws://'+ip+':47475/signaling'),id=crypto.randomUUID();
      second.onopen=()=>second.send(JSON.stringify({protocolVersion:2,type:'hello',requestId:crypto.randomUUID(),timestamp:Date.now(),payload:{sessionId:id,computerName:'busy test'}}));
      second.onmessage=e=>{resolve(JSON.parse(e.data));second.close()};
      second.onerror=()=>reject(Error('Busy test websocket failed'));
    }));
    console.log('Second sender:',JSON.stringify(busy));
    if(busy.payload.code!=='BUSY')throw Error('Second sender was not rejected');
    await page.evaluate(()=>window.stopTest());
    await new Promise(r=>setTimeout(r,1000));
    console.log('H264 receive, rendered frames, BUSY rejection and disconnect verified');
  }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1});
