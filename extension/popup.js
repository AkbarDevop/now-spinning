chrome.runtime.sendMessage({type:'status'}, result => {
  document.querySelector('#status').textContent=result?.message || 'Open YouTube Music.';
  document.querySelector('#track').textContent=[result?.title,result?.artist].filter(Boolean).join(' — ');
});
