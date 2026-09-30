async function main() {
  const { deviceTarget } = await import('./lib/device-target.mjs');
  const t = deviceTarget();   // never a hardcoded PIN
  const ip = t.host.replace(/^https?:\/\//, '');
  const pin = t.pin;
  
  const pairRes = await fetch(`http://${ip}/api/pair`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ pin })
  });
  
  if (!pairRes.ok) {
    console.error('Pairing failed:', pairRes.status, await pairRes.text());
    return;
  }
  
  const cookie = pairRes.headers.get('set-cookie');
  const headers = { 'Cookie': cookie };
  
  console.log('\nSearching for "einstein" in /data/anima/learned/it.jsonl:');
  const readRes = await fetch(`http://${ip}/api/fs/read?path=/data/anima/learned/it.jsonl`, { headers });
  if (readRes.ok) {
    const text = await readRes.text();
    const lines = text.split('\n');
    let found = false;
    for (const line of lines) {
      if (line.toLowerCase().includes('einstein')) {
        console.log(line);
        found = true;
      }
    }
    if (!found) {
      console.log('Not found in it.jsonl!');
    }
  } else {
    console.error('Failed to read it.jsonl:', readRes.status, await readRes.text());
  }
}

main().catch(console.error);
