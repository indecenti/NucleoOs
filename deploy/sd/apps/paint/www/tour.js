// tour.js — Paint's first-run guided tutorial. Bilingual (IT/EN), spotlight + step cards, and — crucially —
// it VERIFIES the device is ready (WebGPU? SDXS model on SD? save target?) and WARNS the user with concrete
// next steps. Self-contained: it injects its own CSS, queries Paint's DOM by id, and runs the capability
// checks itself (probeDiffusion + a manifest fetch). DOM-only; no coupling beyond a few element ids.
//
// startTour({ lang, force, modelDir, inShell, openAtelier }):
//   force=false → shows only on first run (localStorage 'paint.tour.done'); force=true → always (Help menu).
// showShortcuts(lang): the keyboard cheat-sheet (also reachable from Help).

const T = {
  it: {
    skip: 'Salta', back: 'Indietro', next: 'Avanti', done: 'Fatto', start: 'Inizia il tour', later: 'Più tardi',
    checking: 'Verifico…',
    steps: [
      { title: '👋 Benvenuto in Paint', body: 'Editor di immagini con generazione AI sul dispositivo. Ti accompagno in 9 passi — strumenti, trasparenza, e come configurare il modello. Dura un minuto.' },
      { sel: '#tools', title: '🛠️ Gli strumenti', body: 'Pennello (B), Linea (L), Rettangolo (R), Ellisse (E), Gomma, Secchiello, Contagocce, Selezione (M), Bacchetta magica (W) e Testo (T).' },
      { sel: '.color-pair', title: '🎨 Colori', body: 'Colore primario (tratto) e secondario (riempimento). Scambiali con il tasto X. La tavolozza accanto è per i colori rapidi.' },
      { sel: '#optsbar', title: '⚙️ Opzioni dello strumento', body: 'Questa barra cambia in base allo strumento attivo: riempimento delle forme, tolleranza del secchiello/bacchetta, dimensione del testo.' },
      { sel: '#panel', title: '🧰 Pannello Strumenti', body: 'Qui ci sono Selezione, Sfondo, Regolazioni (luminosità/contrasto/saturazione e filtri) e Trasforma. Premi Tab per nasconderlo e avere più tela.' },
      { sel: '.psec[data-sec="bg"]', title: '✂️ Rimozione sfondo', body: 'Apri questa sezione: «Rimuovi sfondo (auto)» rende trasparente lo sfondo uniforme. Oppure scegli la 🪄 bacchetta e clicca lo sfondo. La trasparenza è PNG reale (salvi un .png con alpha).' },
      { sel: '#rulerTop', title: '📏 Righelli e zoom', body: 'I righelli mostrano le coordinate in pixel. Zoom con Ctrl+rotella o i pulsanti in basso; griglia e righelli dal menu Vista.' },
      { sel: '#atelierBtn', title: '✨ Atelier — generazione AI', body: 'Genera immagini da testo o da uno schizzo: gira sulla GPU del tuo browser, non nel cloud. Ora verifico se il tuo dispositivo è pronto…' },
      { title: '🔎 Il tuo dispositivo è pronto?', body: 'Controllo WebGPU, modello e cache locale:', check: true },
      { sel: '[data-menu="file"]', title: '💾 Salva e concludi', body: 'Salva con Ctrl+S (PNG con trasparenza). Puoi rivedere questo tutorial da Aiuto → Tutorial guidato. Buon lavoro! 🎉' },
    ],
    chkWebgpuOk: (mb) => `WebGPU disponibile${mb ? ` (~${mb} MB)` : ''} — generazione veloce.`,
    chkWebgpuWasm: 'Niente WebGPU: userà la CPU (WASM), lento ma funziona. Usa Chrome/Edge desktop per il meglio.',
    chkModelOk: "Modello SDXS presente sull'SD del Cardputer — pronto.",
    chkModelNo: "Modello non sull'SD del Cardputer. Puoi comunque scaricarlo da internet con il pulsante sotto (553 MB, GitHub). Per installarlo sull'SD: node tools/sdxs/provision-lsb.mjs poi sd-sync.ps1.",
    chkCacheOk: 'Cache locale presente — caricamento istantaneo (niente WiFi necessario).',
    chkCacheNo: (mb) => `Prima apertura: ${mb} MB scaricati da internet (GitHub) o dal Cardputer via WiFi, poi salvati nel browser. Le volte successive è locale e immediato.`,
    chkCacheOpen: 'Apri Atelier e scarica ora',
    chkSaveOk: 'Salvataggio su Cardputer attivo.',
    chkSaveNo: 'Fuori dalla shell: il salvataggio su Cardputer funziona solo nella shell NucleoOS.',
    scTitle: 'Scorciatoie da tastiera', scClose: 'Chiudi',
    sc: [['B / L / R / E', 'Pennello / Linea / Rettangolo / Ellisse'], ['M / W / T', 'Selezione / Bacchetta / Testo'],
      ['X', 'Scambia colori primario/secondario'], ['Ctrl+Z / Ctrl+Y', 'Annulla / Ripeti'], ['Ctrl+S', 'Salva'],
      ['Ctrl+rotella', 'Zoom'], ['Tab', 'Mostra/nascondi pannello'], ['Canc', 'Cancella tutto'], ['Esc', 'Chiudi finestra/tour']],
  },
  en: {
    skip: 'Skip', back: 'Back', next: 'Next', done: 'Done', start: 'Start tour', later: 'Later',
    checking: 'Checking…',
    steps: [
      { title: '👋 Welcome to Paint', body: 'An image editor with on-device AI generation. I\'ll walk you through 9 steps — tools, transparency, and how to set up the model. Takes a minute.' },
      { sel: '#tools', title: '🛠️ The tools', body: 'Brush (B), Line (L), Rectangle (R), Ellipse (E), Eraser, Bucket, Eyedropper, Select (M), Magic wand (W) and Text (T).' },
      { sel: '.color-pair', title: '🎨 Colors', body: 'Primary color (stroke) and secondary (fill). Swap them with the X key. The palette next to it is for quick colors.' },
      { sel: '#optsbar', title: '⚙️ Tool options', body: 'This bar changes with the active tool: shape fill, bucket/wand tolerance, text size.' },
      { sel: '#panel', title: '🧰 Tools panel', body: 'Selection, Background, Adjustments (brightness/contrast/saturation and filters) and Transform live here. Press Tab to hide it for more canvas.' },
      { sel: '.psec[data-sec="bg"]', title: '✂️ Background removal', body: 'Open this section: "Remove background (auto)" makes a uniform background transparent. Or pick the 🪄 wand and click the background. Transparency is real PNG (you save a .png with alpha).' },
      { sel: '#rulerTop', title: '📏 Rulers and zoom', body: 'Rulers show pixel coordinates. Zoom with Ctrl+wheel or the buttons below; grid and rulers from the View menu.' },
      { sel: '#atelierBtn', title: '✨ Atelier — AI generation', body: 'Generate images from text or a sketch: it runs on your browser GPU, not the cloud. Let me check whether your device is ready…' },
      { title: '🔎 Is your device ready?', body: 'Checking WebGPU, model and local cache:', check: true },
      { sel: '[data-menu="file"]', title: '💾 Save and finish', body: 'Save with Ctrl+S (PNG with transparency). You can replay this tutorial from Help → Guided tutorial. Have fun! 🎉' },
    ],
    chkWebgpuOk: (mb) => `WebGPU available${mb ? ` (~${mb} MB)` : ''} — fast generation.`,
    chkWebgpuWasm: 'No WebGPU: will use CPU (WASM), slow but works. Use desktop Chrome/Edge for best results.',
    chkModelOk: 'SDXS model present on the Cardputer SD — ready.',
    chkModelNo: 'Model not on the Cardputer SD. You can still download it from the internet with the button below (553 MB, GitHub). To install it on the SD: node tools/sdxs/provision-lsb.mjs then sd-sync.ps1.',
    chkCacheOk: 'Local cache present — instant load (no WiFi needed).',
    chkCacheNo: (mb) => `First open: ${mb} MB downloaded from the internet (GitHub) or the Cardputer via WiFi, then saved in the browser. Subsequent opens are instant and local.`,
    chkCacheOpen: 'Open Atelier & download now',
    chkSaveOk: 'Saving to the Cardputer is active.',
    chkSaveNo: 'Outside the shell: saving to the Cardputer only works in the NucleoOS shell.',
    scTitle: 'Keyboard shortcuts', scClose: 'Close',
    sc: [['B / L / R / E', 'Brush / Line / Rectangle / Ellipse'], ['M / W / T', 'Select / Wand / Text'],
      ['X', 'Swap primary/secondary colors'], ['Ctrl+Z / Ctrl+Y', 'Undo / Redo'], ['Ctrl+S', 'Save'],
      ['Ctrl+wheel', 'Zoom'], ['Tab', 'Toggle panel'], ['Del', 'Clear all'], ['Esc', 'Close window/tour']],
  },
};

// Check whether at least one chunk of the SDXS model is already in IndexedDB.
// Uses the same DB name as model-cache.js: sdxs-cache-{revision}.
async function hasModelCached(revision) {
  if (typeof indexedDB === 'undefined') return false;
  try {
    const db = await new Promise((res, rej) => {
      const req = indexedDB.open(`sdxs-cache-${revision}`, 1);
      req.onupgradeneeded = e => e.target.result.createObjectStore('chunks');
      req.onsuccess = e => res(e.target.result);
      req.onerror = () => rej(req.error);
    });
    const count = await new Promise(res => {
      try {
        const tx  = db.transaction('chunks', 'readonly');
        const req = tx.objectStore('chunks').count();
        req.onsuccess = () => res(req.result);
        req.onerror   = () => res(0);
      } catch { res(0); }
    });
    db.close();
    return count > 0;
  } catch { return false; }
}

let styled = false;
function injectStyle() {
  if (styled) return; styled = true;
  const s = document.createElement('style'); s.id = 'tour-style';
  s.textContent = `
  .tour-back{position:fixed;inset:0;z-index:9000;font-family:'Segoe UI',system-ui,sans-serif}
  .tour-dim{position:fixed;background:rgba(3,6,14,.72);transition:all .22s cubic-bezier(.4,0,.2,1)}
  .tour-ring{position:fixed;border:2px solid #4ea1ff;border-radius:10px;box-shadow:0 0 18px rgba(78,161,255,.5);transition:all .22s cubic-bezier(.4,0,.2,1);pointer-events:none}
  .tour-card{position:fixed;width:min(330px,92vw);background:#0f1521;color:#f3f4f6;border:1px solid rgba(255,255,255,.12);border-radius:14px;box-shadow:0 24px 60px rgba(0,0,0,.6);padding:16px;transition:all .25s cubic-bezier(.4,0,.2,1);z-index:9001}
  .tour-card h3{margin:0 0 8px;font-size:15px}
  .tour-card p{margin:0;font-size:13px;line-height:1.5;color:#cdd6e4}
  .tour-checks{margin-top:12px;display:flex;flex-direction:column;gap:8px}
  .tour-chk{display:flex;gap:8px;font-size:12.5px;line-height:1.4;align-items:flex-start}
  .tour-chk .ic{flex:none;width:18px;text-align:center}
  .tour-chk.ok .ic{color:#5fe39a}.tour-chk.warn .ic{color:#ffd86b}.tour-chk.wait .ic{color:#93a1b8}
  .tour-nav{display:flex;align-items:center;gap:8px;margin-top:16px}
  .tour-dots{display:flex;gap:5px;flex:1}
  .tour-dots i{width:6px;height:6px;border-radius:50%;background:rgba(255,255,255,.22)}
  .tour-dots i.on{background:#4ea1ff;width:16px;border-radius:4px}
  .tour-btn{background:#1a2433;border:1px solid rgba(255,255,255,.12);color:#f3f4f6;border-radius:8px;padding:8px 14px;cursor:pointer;font-size:13px}
  .tour-btn:hover{background:#243044}
  .tour-btn.primary{background:linear-gradient(135deg,#6d5efc,#4ea1ff);border:none;color:#fff;font-weight:700}
  .tour-skip{background:none;border:none;color:#93a1b8;cursor:pointer;font-size:12px;text-decoration:underline}
  .tour-skip:hover{color:#f3f4f6}`;
  document.head.appendChild(s);
}

const lget = (k) => { try { return localStorage.getItem(k); } catch { return null; } };
const lset = (k, v) => { try { localStorage.setItem(k, v); } catch {} };

export function startTour({ lang = 'it', force = false, modelDir = '', inShell = false } = {}) {
  if (!force && lget('paint.tour.done')) return;
  injectStyle();
  const t = T[lang === 'en' ? 'en' : 'it'];
  const steps = t.steps;
  let i = 0, back = null;

  function cleanup(markDone) { if (markDone) lset('paint.tour.done', '1'); window.removeEventListener('keydown', onKey, true); if (back) back.remove(); back = null; }
  function onKey(e) {
    if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); cleanup(true); }
    else if (e.key === 'Enter' || e.key === 'ArrowRight') { e.preventDefault(); e.stopPropagation(); go(1); }
    else if (e.key === 'ArrowLeft') { e.preventDefault(); e.stopPropagation(); go(-1); }
  }
  function go(d) { const n = i + d; if (n < 0) return; if (n >= steps.length) { cleanup(true); return; } i = n; render(); }

  // Spotlight via FOUR dimming rectangles around the target (cheap to paint — good for the low-power client)
  // plus a highlight ring. No giant box-shadow.
  function place(card, target) {
    const W = innerWidth, H = innerHeight, m = 14;
    const dT = back.querySelector('[data-d=t]'), dB = back.querySelector('[data-d=b]'), dL = back.querySelector('[data-d=l]'), dR = back.querySelector('[data-d=r]'), ring = back.querySelector('.tour-ring');
    let hx, hy, hw, hh;
    if (!target) { hx = W / 2; hy = H / 2; hw = 0; hh = 0; ring.style.opacity = '0'; }
    else { const r = target.getBoundingClientRect(); hx = r.left - 5; hy = r.top - 5; hw = r.width + 10; hh = r.height + 10;
      ring.style.opacity = '1'; ring.style.left = hx + 'px'; ring.style.top = hy + 'px'; ring.style.width = hw + 'px'; ring.style.height = hh + 'px'; }
    const px = (n) => Math.max(0, n) + 'px';
    dT.style.cssText = `left:0;top:0;width:${W}px;height:${px(hy)}`;
    dB.style.cssText = `left:0;top:${hy + hh}px;width:${W}px;height:${px(H - (hy + hh))}`;
    dL.style.cssText = `left:0;top:${hy}px;width:${px(hx)};height:${hh}px`;
    dR.style.cssText = `left:${hx + hw}px;top:${hy}px;width:${px(W - (hx + hw))};height:${hh}px`;
    const cw = card.offsetWidth, ch = card.offsetHeight;
    if (!target) { card.style.left = (W - cw) / 2 + 'px'; card.style.top = (H - ch) / 2 + 'px'; return; }
    const r = target.getBoundingClientRect(); let top = r.bottom + m, left = r.left;
    if (top + ch > H - 8) top = Math.max(8, r.top - ch - m);                    // flip above if no room below
    left = Math.min(Math.max(8, left), W - cw - 8);
    card.style.left = left + 'px'; card.style.top = Math.max(8, top) + 'px';
  }

  async function runChecks(box, cleanup) {
    const add = (cls, ic, txt) => { const d = document.createElement('div'); d.className = 'tour-chk ' + cls; d.innerHTML = `<span class="ic">${ic}</span><span>${txt}</span>`; box.appendChild(d); return d; };
    // 4 fixed rows: WebGPU · model on SD · local cache · save target
    const wG = add('wait', '⏳', t.checking);
    const wM = add('wait', '⏳', t.checking);
    const wC = add('wait', '⏳', t.checking);
    const wSv = add(inShell ? 'ok' : 'warn', inShell ? '✓' : '⚠', inShell ? t.chkSaveOk : t.chkSaveNo);
    void wSv;

    // 1) WebGPU — raced against a 4s timeout so a slow/flaky adapter can never hang the tutorial.
    try { const m = await import('./diffusion/webgpu-probe.js');
      const p = await Promise.race([m.probeDiffusion({}), new Promise((_, rej) => setTimeout(() => rej(new Error('timeout')), 4000))]);
      if (p.ep === 'webgpu') { wG.className = 'tour-chk ok'; wG.innerHTML = `<span class="ic">✓</span><span>${t.chkWebgpuOk(p.vramMB)}</span>`; }
      else { wG.className = 'tour-chk warn'; wG.innerHTML = `<span class="ic">⚠</span><span>${t.chkWebgpuWasm}</span>`; }
    } catch { wG.className = 'tour-chk warn'; wG.innerHTML = `<span class="ic">⚠</span><span>${t.chkWebgpuWasm}</span>`; }

    // 2) Model on SD
    let hasModel = false; let manifest = null;
    try {
      const r = await fetch('/api/fs/read?path=' + encodeURIComponent(modelDir + '/manifest.json') + '&t=' + Date.now());
      if (r.ok) { manifest = await r.json(); hasModel = true; }
    } catch {}
    wM.className = hasModel ? 'tour-chk ok' : 'tour-chk warn';
    wM.innerHTML = `<span class="ic">${hasModel ? '✓' : '⚠'}</span><span>${hasModel ? t.chkModelOk : t.chkModelNo}</span>`;

    // 3) Local cache — revision matches model-cache.js and tryReal() hardcoded revision.
    // count > 0 is true for both SD-chunked entries AND internet-assembled 'net-*' entries.
    const REV = '2024-12-lsb';
    const revision = (manifest && manifest.revision) || REV;
    const mb = Math.round(((manifest && manifest.totalBytes) || 553e6) / 1e6);
    const isCached = await hasModelCached(revision);
    if (isCached) {
      wC.className = 'tour-chk ok';
      wC.innerHTML = `<span class="ic">✓</span><span>${t.chkCacheOk}</span>`;
    } else {
      // Not yet cached — show download explanation + shortcut button.
      // Works even without the Cardputer: tryReal() tries internet (GitHub) first.
      wC.className = 'tour-chk wait';
      wC.innerHTML = `<span class="ic">⬇</span><span>${t.chkCacheNo(mb)}</span>`;
      const btn = document.createElement('button');
      btn.className = 'tour-btn primary';
      btn.style.cssText = 'margin-top:10px;font-size:12px;padding:6px 14px;width:100%';
      btn.textContent = t.chkCacheOpen;
      btn.onclick = () => { if (cleanup) cleanup(true); setTimeout(() => { if (window.openAtelier) window.openAtelier('text'); }, 80); };
      wC.appendChild(document.createElement('br'));
      wC.appendChild(btn);
    }
  }

  function render() {
    if (!back) { back = document.createElement('div'); back.className = 'tour-back'; document.body.appendChild(back); window.addEventListener('keydown', onKey, true); }
    const st = steps[i], target = st.sel ? document.querySelector(st.sel) : null;
    if (st.sel && target) target.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
    back.innerHTML = `<div class="tour-dim" data-d="t"></div><div class="tour-dim" data-d="b"></div><div class="tour-dim" data-d="l"></div><div class="tour-dim" data-d="r"></div><div class="tour-ring"></div>
      <div class="tour-card" role="dialog" aria-label="tutorial">
        <h3>${st.title}</h3><p>${st.body}</p>
        <div class="tour-checks" ${st.check ? '' : 'style="display:none"'}></div>
        <div class="tour-nav">
          <div class="tour-dots">${steps.map((_, k) => `<i class="${k === i ? 'on' : ''}"></i>`).join('')}</div>
          ${i > 0 ? `<button class="tour-btn" data-a="back">${t.back}</button>` : `<button class="tour-skip" data-a="skip">${t.skip}</button>`}
          <button class="tour-btn primary" data-a="next">${i === steps.length - 1 ? t.done : t.next}</button>
        </div>
      </div>`;
    const card = back.querySelector('.tour-card');
    place(card, target);   // synchronous (reading offsetWidth forces layout) — robust even if rAF is throttled
    requestAnimationFrame(() => place(card, target));   // and re-place next frame in case fonts/scroll shifted it
    if (st.check) runChecks(back.querySelector('.tour-checks'), cleanup);
    back.querySelectorAll('[data-a]').forEach((b) => b.onclick = () => { const a = b.dataset.a; if (a === 'next') go(1); else if (a === 'back') go(-1); else cleanup(true); });
  }
  render();
}

export function showShortcuts(lang = 'it') {
  injectStyle();
  const t = T[lang === 'en' ? 'en' : 'it'];
  const back = document.createElement('div'); back.className = 'tour-back';
  back.innerHTML = `<div class="tour-card" style="left:50%;top:50%;transform:translate(-50%,-50%);width:min(380px,92vw)">
    <h3>⌨️ ${t.scTitle}</h3>
    <div style="margin-top:10px;display:flex;flex-direction:column;gap:7px">
      ${t.sc.map(([k, d]) => `<div style="display:flex;gap:12px;font-size:13px"><b style="flex:none;min-width:108px;color:#9cd0ff;font-variant-numeric:tabular-nums">${k}</b><span style="color:#cdd6e4">${d}</span></div>`).join('')}
    </div>
    <div class="tour-nav"><div class="tour-dots"></div><button class="tour-btn primary" data-x>${t.scClose}</button></div>
  </div>`;
  const close = () => { back.remove(); window.removeEventListener('keydown', onKey, true); };
  const onKey = (e) => { if (e.key === 'Escape') close(); };
  back.addEventListener('click', (e) => { if (e.target === back) close(); });
  back.querySelector('[data-x]').onclick = close;
  window.addEventListener('keydown', onKey, true);
  document.body.appendChild(back);
}
