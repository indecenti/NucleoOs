// Auto-diagnostic: waits for the device on WiFi (after USB unplug), forces the Wikipedia tier
// (Grok off), triggers a query that hangs, and dumps the http phase trace read back over WiFi.
const H = 'http://192.168.0.166';
const s = ms => new Promise(r => setTimeout(r, ms));
async function pair() {
  const r = await fetch(H + '/api/pair', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ pin: '689614' }) });
  const c = r.headers.get('set-cookie');
  return c ? c.split(';')[0] : '';
}
(async () => {
  console.log('STACCA L\'USB ORA — aspetto il device su WiFi .166...');
  let up = false;
  for (let i = 0; i < 150; i++) { try { const r = await fetch(H + '/api/status', { signal: AbortSignal.timeout(3000) }); if (r.ok) { up = true; break; } } catch {} await s(3000); }
  if (!up) { console.log('Device mai arrivato su WiFi.'); return; }
  const st = await (await fetch(H + '/api/status')).json();
  console.log('device UP v' + st.version + ' ip=' + (st.network && st.network.ip));
  const cookie = await pair(); const hdr = { cookie };
  await fetch(H + '/api/fs/delete?path=/data/anima/.httptrace', { method: 'POST', headers: hdr }).catch(() => {});
  await fetch(H + '/api/fs/write?path=/data/anima/teacher.json', { method: 'POST', headers: hdr, body: '{"key":"","base":"https://api.groq.com/openai/v1","model":"llama-3.1-8b-instant"}' });
  console.log('grok off, trace pulito -> lancio "chi è Garibaldi" (andra in hang ~60s)...');
  const t = Date.now();
  try { const r = await fetch(H + '/api/anima?q=' + encodeURIComponent('chi è Garibaldi') + '&lang=it&mode=on', { signal: AbortSignal.timeout(70000) }); const j = await r.json(); console.log('reply ' + (Date.now() - t) + 'ms: ' + j.reply); }
  catch (e) { console.log('ended ' + (Date.now() - t) + 'ms: ' + e.message); }
  await s(1200);
  const tr = await (await fetch(H + '/api/fs/read?path=/data/anima/.httptrace', { headers: hdr })).text();
  console.log('\n=== TRACE ===\n' + (tr || '(vuoto)'));
})();
