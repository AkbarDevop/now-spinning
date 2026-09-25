// Read only the visible player and its standard HTML media element.
let busy = false;
async function update() {
  if (busy || !chrome.runtime?.id) return;
  const bar = document.querySelector('ytmusic-player-bar');
  const title = bar?.querySelector('.title')?.textContent?.trim();
  const artwork = bar?.querySelector('img.image')?.src;
  if (!title || !artwork) return;
  const media = document.querySelector('video');
  const playing = media ? !media.paused && !media.ended :
    bar.querySelector('#play-pause-button')?.getAttribute('title') === 'Pause';
  const artist = bar.querySelector('.byline-wrapper')?.innerText?.trim().replace(/\s+/g,' ') || '';
  busy = true;
  try { await chrome.runtime.sendMessage({type:'track', title, artist, artwork, playing}); }
  catch (_) { /* An extension reload requires refreshing this music tab. */ }
  finally { busy = false; }
}
// Tap gestures on the display arrive here (double tap = next, triple tap = play/pause).
chrome.runtime.onMessage.addListener((msg) => {
  if (msg?.type !== 'command') return;
  const selector = {next:'.next-button', previous:'.previous-button', toggle:'#play-pause-button'}[msg.command];
  document.querySelector('ytmusic-player-bar')?.querySelector(selector)?.click();
  setTimeout(update, 600);
});
setInterval(update, 2000);
update();
