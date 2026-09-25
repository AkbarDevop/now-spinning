importScripts('pairing.js');
let cachedURL = '', cachedPixels = '', busy = false;
let status = {connected:false, message:'Open YouTube Music and start the Mac bridge.'};

function validArt(value) {
  const u = new URL(value);
  return u.protocol === 'https:' && !u.username && !u.password &&
    (['yt3.googleusercontent.com','lh3.googleusercontent.com','i.ytimg.com'].includes(u.hostname) ||
      u.hostname.endsWith('.ggpht.com'));
}

async function artworkPixels(url) {
  if (!validArt(url)) throw new Error('Unsupported artwork host');
  if (url === cachedURL && cachedPixels) return cachedPixels;
  const response = await fetch(url, {credentials:'omit', redirect:'error', signal:AbortSignal.timeout(5000)});
  if (!response.ok) throw new Error('Could not load artwork');
  const blob = await response.blob();
  if (blob.size > 4*1024*1024 || !blob.type.startsWith('image/')) throw new Error('Invalid artwork image');
  const image = await createImageBitmap(blob);
  const canvas = new OffscreenCanvas(64,64), ctx = canvas.getContext('2d');
  const side = Math.min(image.width,image.height);
  ctx.drawImage(image,(image.width-side)/2,(image.height-side)/2,side,side,0,0,64,64);
  image.close();
  const rgba = ctx.getImageData(0,0,64,64).data, pixels = new Uint8Array(8192);
  for(let i=0;i<4096;i++) {
    const rgb=((rgba[i*4]>>3)<<11)|((rgba[i*4+1]>>2)<<5)|(rgba[i*4+2]>>3);
    pixels[i*2]=rgb&255; pixels[i*2+1]=rgb>>8;
  }
  cachedURL=url; cachedPixels=btoa(String.fromCharCode(...pixels));
  return cachedPixels;
}

chrome.runtime.onMessage.addListener((msg,sender,reply) => {
  if(msg?.type==='status') { reply(status); return; }
  if(msg?.type!=='track' || !sender.tab || new URL(sender.url).origin!=='https://music.youtube.com') return;
  if(busy) { reply({busy:true}); return; }
  busy=true;
  (async () => {
    try {
      const pixels=await artworkPixels(msg.artwork);
      const response=await fetch('http://127.0.0.1:18765/frame',{
        method:'POST', headers:{'Content-Type':'application/json','X-Matrix-Token':MATRIX_TOKEN},
        body:JSON.stringify({pixels,playing:msg.playing===true,title:String(msg.title).slice(0,240),artist:String(msg.artist).slice(0,240)}),
        signal:AbortSignal.timeout(6500)
      });
      const data=await response.json();
      if(!response.ok) throw new Error(data.error || 'Bridge unavailable');
      if(['next','toggle','previous'].includes(data.command))
        chrome.tabs.sendMessage(sender.tab.id,{type:'command',command:data.command}).catch(()=>{});
      status={connected:true,message:msg.playing?'Playing':'Paused',title:msg.title,artist:msg.artist};
      chrome.action.setBadgeText({text:'ON'}); chrome.action.setBadgeBackgroundColor({color:'#167a4b'});
    } catch(e) {
      status={connected:false,message:e.message};
      chrome.action.setBadgeText({text:'!'}); chrome.action.setBadgeBackgroundColor({color:'#a84319'});
    } finally { busy=false; reply(status); }
  })();
  return true;
});
