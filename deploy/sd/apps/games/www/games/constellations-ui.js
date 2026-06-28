// constellations-ui.js — the triple-A DOM/CSS overlay for Costellazioni 3D. Pure DOM (no imports);
// everything it needs arrives via the `model` passed to paint() (run + generated sector/missions +
// econ/shop helpers). It owns: the status strip, tab strip, the five hub screens (Plancia/Mappa/
// Mercato/Cantiere/Missioni), the combat HUD, debrief, toasts, and the load/new-run/conflict modals.
// Mouse works by synthesizing keydown events the harness already forwards (click == keyboard), so
// there is ONE input path. Diff-gated repaint: the DOM is rebuilt only when a state key changes.

const FAC = ['#4d7bd0', '#28aa96', '#c06434', '#a882e6'];
const ECO = ['#7fc77a', '#c9a25a', '#8aa0b8', '#9b8cff', '#5ad0c0'];
const MT = [['Pattuglia', 'Patrol'], ['Taglia', 'Bounty'], ['Scorta', 'Escort'], ['Difesa', 'Defend']];
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

const CSS = `
[data-cz]{position:absolute;inset:0;pointer-events:none;z-index:30;font-family:system-ui,'Segoe UI',sans-serif;color:#e6edf3;font-variant-numeric:tabular-nums;
 --acc:#60cee8;--accg:rgba(96,206,232,.55);--glass:rgba(10,16,30,.62);--stroke:rgba(120,170,210,.18);--edge:#243049;--hull:#0c1426;--good:#76e68c;--warn:#ffbe40;--bad:#ff5c50;--lo:#9fb0c4;--dim:#7c899b}
[data-cz] *{box-sizing:border-box}
.cz-panel{background:var(--glass);border:1px solid var(--stroke);border-radius:16px;backdrop-filter:blur(14px) saturate(1.1);box-shadow:0 18px 50px rgba(0,0,0,.5),inset 0 1px 0 rgba(255,255,255,.06)}
@supports not (backdrop-filter:blur(2px)){.cz-panel{background:#0e1830}}
.cz-top{position:absolute;top:14px;left:14px;right:62px;height:46px;display:flex;align-items:center;gap:16px;padding:0 18px;border-radius:999px;font-size:13px;font-weight:700}
.cz-top .cr{color:var(--warn);font-size:15px} .cz-top .sec{color:var(--lo)} .cz-pip{display:inline-block;width:8px;height:8px;border-radius:50%;margin:0 2px;border:1px solid var(--acc)} .cz-pip.on{background:var(--acc);box-shadow:0 0 8px var(--accg)}
.cz-top .vit{margin-left:auto;display:flex;gap:14px;color:var(--lo);font-size:12px}
.cz-tabs{position:absolute;top:70px;left:14px;right:14px;display:flex;gap:8px;pointer-events:auto}
.cz-tab{flex:0 0 auto;padding:8px 16px;border-radius:999px;border:1px solid var(--edge);background:var(--hull);color:var(--lo);font-weight:700;font-size:13px;cursor:pointer;transition:.14s}
.cz-tab:hover{border-color:#3b7e90}.cz-tab.on{color:#06121c;background:var(--acc);border-color:var(--acc);box-shadow:0 0 18px var(--accg)}
.cz-body{position:absolute;top:112px;left:14px;right:14px;bottom:50px;display:flex;gap:14px;animation:sweep .18s cubic-bezier(.22,.61,.36,1)}
@keyframes sweep{from{opacity:0;transform:translateY(10px)}to{opacity:1;transform:none}}
.cz-col{display:flex;flex-direction:column;gap:12px;min-width:0}
.cz-card{padding:16px;flex:0 0 auto}.cz-card h3{margin:0 0 10px;font-size:13px;letter-spacing:.08em;color:var(--lo);text-transform:uppercase;font-weight:800}
.cz-list{display:flex;flex-direction:column;gap:8px;overflow:auto;pointer-events:auto;padding-right:2px}
.cz-row{position:relative;min-height:46px;display:flex;align-items:center;gap:12px;padding:10px 14px;border:1px solid var(--edge);border-radius:12px;background:var(--hull);cursor:pointer;transition:transform .12s,border-color .12s,background .12s}
.cz-row:hover{border-color:#3b7e90}
.cz-row.sel{border-color:var(--acc);background:rgba(40,70,120,.5);box-shadow:0 0 22px var(--accg),inset 0 0 0 1px var(--acc);transform:translateX(2px) scale(1.012)}
.cz-row.sel::before{content:'';position:absolute;left:0;top:6px;bottom:6px;width:3px;border-radius:3px;background:var(--acc);box-shadow:0 0 10px var(--accg)}
.cz-row.dis{opacity:.42;pointer-events:none} .cz-row b{font-size:15px} .cz-row .r{margin-left:auto;font-weight:800}
.cz-cell{padding:3px 9px;border-radius:8px;cursor:pointer;border:1px solid transparent;transition:.12s}
.cz-cell:hover{border-color:var(--acc);background:rgba(96,206,232,.12)}
.cz-cell.on{border-color:var(--acc);box-shadow:0 0 10px var(--accg)}
.cz-sub{color:var(--dim);font-size:12px;margin-top:2px}
.cz-chip{display:inline-block;padding:2px 8px;border-radius:999px;font-size:11px;font-weight:700;background:rgba(255,255,255,.08)}
.cz-bar{width:72px;height:6px;border-radius:3px;background:#16203a;overflow:hidden;display:inline-block}.cz-bar i{display:block;height:100%;background:var(--good)}
.cz-pips{display:inline-flex;gap:3px}.cz-pips s{width:10px;height:5px;border-radius:2px;background:#16203a}.cz-pips s.on{background:var(--good)}
.cz-rep{display:grid;grid-template-columns:auto 1fr auto;gap:6px 8px;align-items:center;font-size:12px}
.cz-rep .b{height:7px;border-radius:4px;background:#16203a;position:relative}.cz-rep .b i{position:absolute;top:0;bottom:0;left:50%;background:var(--good)}
.cz-hint{position:absolute;left:14px;right:14px;bottom:10px;height:32px;display:flex;align-items:center;justify-content:center;gap:18px;font-size:12px;color:var(--lo)}
.cz-hint kbd{background:#10203a;border:1px solid var(--edge);border-radius:6px;padding:1px 7px;font-family:inherit;color:#cfd8e6;margin:0 3px}
.cz-toast{position:absolute;left:50%;transform:translateX(-50%);bottom:54px;padding:8px 18px;border-radius:999px;font-weight:700;font-size:14px;pointer-events:none;animation:sweep .18s}
.cz-toast.bad{background:rgba(60,16,16,.85);color:var(--bad);border:1px solid var(--bad)}.cz-toast.good{background:rgba(16,48,28,.85);color:var(--good);border:1px solid var(--good)}.cz-toast.warn{background:rgba(60,46,12,.85);color:var(--warn);border:1px solid var(--warn)}
.cz-map{position:relative;flex:1 1 auto;aspect-ratio:1/1;max-height:100%;align-self:center}
.cz-node{position:absolute;width:14px;height:14px;border-radius:50%;transform:translate(-50%,-50%);border:1px solid #05060f;cursor:pointer;pointer-events:auto}
.cz-node.cur{box-shadow:0 0 0 4px rgba(118,230,140,.25);animation:pulse 1.4s infinite}
.cz-node.tgt{box-shadow:0 0 0 3px var(--acc),0 0 16px var(--accg)}
.cz-node .bk{position:absolute;inset:-5px;border-radius:50%;border:2px solid}
.cz-node .lbl{position:absolute;left:50%;top:16px;transform:translateX(-50%);white-space:nowrap;font-size:10px;color:var(--lo);text-shadow:0 0 6px #000}
@keyframes pulse{0%,100%{box-shadow:0 0 0 4px rgba(118,230,140,.28)}50%{box-shadow:0 0 0 8px rgba(118,230,140,.08)}}
.cz-modal{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;flex-direction:column;text-align:center;gap:14px}
.cz-modal .cz-card{max-width:440px;align-items:center;display:flex;flex-direction:column;gap:12px}
.cz-spin{width:160px;height:6px;border-radius:3px;background:#16203a;overflow:hidden}.cz-spin i{display:block;width:40%;height:100%;background:var(--acc);animation:slide 1.1s infinite}
@keyframes slide{from{margin-left:-40%}to{margin-left:100%}}
.cz-cmb{position:absolute;inset:0;cursor:none;font-family:ui-monospace,'Cascadia Mono','Consolas',monospace}
[data-hub]{cursor:default}
.cz-crt{position:absolute;inset:0;pointer-events:none;background:radial-gradient(ellipse 82% 78% at 50% 48%,transparent 66%,rgba(2,10,20,.45) 100%)}
.cz-frame i{position:absolute;width:46px;height:46px;border:2px solid rgba(94,230,255,.45);box-shadow:0 0 12px rgba(94,230,255,.25)}
.cz-frame i:nth-child(1){top:14px;left:14px;border-right:0;border-bottom:0}
.cz-frame i:nth-child(2){top:14px;right:14px;border-left:0;border-bottom:0}
.cz-frame i:nth-child(3){bottom:14px;left:14px;border-right:0;border-top:0}
.cz-frame i:nth-child(4){bottom:14px;right:14px;border-left:0;border-top:0}
.cz-wave{position:absolute;top:20px;left:50%;transform:translateX(-50%);display:flex;gap:6px;align-items:center}
.cz-wave s{width:20px;height:8px;background:rgba(94,230,255,.12);border:1px solid rgba(94,230,255,.4);transform:skewX(-18deg)}
.cz-wave s.on{background:#5ee6ff;box-shadow:0 0 10px rgba(94,230,255,.8)}
.cz-kills{position:absolute;top:18px;right:64px;font-weight:700;font-size:13px;letter-spacing:.14em;color:#5ee6ff;text-shadow:0 0 8px rgba(94,230,255,.6)}.cz-kills b{font-size:17px}
.cz-tgt{position:absolute;left:50%;bottom:34%;transform:translateX(-50%);font-weight:700;font-size:14px;letter-spacing:.22em;color:rgba(94,230,255,.7);text-shadow:0 0 8px rgba(94,230,255,.5)}
.cz-tgt.lock{color:#ff5c50;text-shadow:0 0 12px rgba(255,92,80,.9);animation:tgtblink .5s steps(2,end) infinite}
@keyframes tgtblink{50%{opacity:.4}}
.cz-gauge{position:absolute;bottom:26px;display:flex;align-items:center;gap:11px;font-weight:700;font-size:13px;letter-spacing:.14em;color:#5ee6ff;text-shadow:0 0 7px rgba(94,230,255,.55)}
.cz-gauge.sh{left:30px}.cz-gauge.hu{right:30px;flex-direction:row-reverse}
.cz-gauge label{opacity:.9}
.cz-gauge .seg{position:relative;width:170px;height:16px;border:1px solid currentColor;box-shadow:0 0 9px rgba(94,230,255,.25),inset 0 0 6px rgba(94,230,255,.12)}
.cz-gauge .seg i{position:absolute;left:1px;top:1px;bottom:1px;width:0;background:currentColor;box-shadow:0 0 10px currentColor;transition:width .12s}
.cz-gauge.hu .seg i{right:1px;left:auto}
.cz-gauge .seg::after{content:'';position:absolute;inset:0;background:repeating-linear-gradient(90deg,transparent 0 14px,#05070d 14px 16px)}
.cz-gauge b{min-width:34px;font-size:17px;text-align:center}
.cz-gauge.hu{color:#76e68c;text-shadow:0 0 7px rgba(118,230,140,.55)}
.cz-gauge.hu.low{color:#ff5c50;text-shadow:0 0 9px rgba(255,92,80,.7);animation:fxbad 1s infinite}
.cz-miss{position:absolute;left:50%;bottom:58px;transform:translateX(-50%);font-weight:700;font-size:12px;letter-spacing:.18em;color:#ffbe40;text-shadow:0 0 7px rgba(255,190,64,.5)}
.cz-miss span{font-size:15px;letter-spacing:.12em}
.cz-banner{position:absolute;top:36%;left:0;right:0;text-align:center;font-weight:800;font-size:clamp(22px,5vw,42px);color:var(--acc);opacity:0;transition:opacity .2s;text-shadow:0 0 22px var(--accg)}
.cz-deb{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;flex-direction:column;text-align:center;gap:12px}
.cz-deb .t{font-weight:800;font-size:clamp(30px,8vw,64px)}
.cz-btn{padding:10px 22px;border-radius:12px;border:1px solid var(--acc);background:rgba(40,70,120,.4);color:#e6edf3;font-weight:800;font-size:15px;pointer-events:auto;cursor:pointer}
.cz-fx{position:absolute;inset:0;pointer-events:none;opacity:0;border-radius:18px}
.cz-fx.pop{animation:fxpop .4s ease-out}@keyframes fxpop{0%{opacity:1;background:radial-gradient(circle at 50% 62%,rgba(118,230,140,.22),transparent 58%)}100%{opacity:0}}
.cz-fx.warp{animation:fxwarp .6s ease-out}@keyframes fxwarp{0%{opacity:1;box-shadow:inset 0 0 140px 36px var(--accg)}100%{opacity:0;box-shadow:inset 0 0 0 0 transparent}}
.cz-fx.ignite{animation:fxign .8s ease-out}@keyframes fxign{0%{opacity:1;background:radial-gradient(circle at 50% 50%,rgba(255,190,64,.28),transparent 55%)}100%{opacity:0}}
.cz-fx.sector{animation:fxsec 1.5s ease-out}@keyframes fxsec{0%{opacity:1;background:radial-gradient(circle,rgba(96,206,232,.42),transparent 62%)}45%{opacity:.55}100%{opacity:0}}
.cz-fx.shake{animation:fxbad .26s ease-out}@keyframes fxbad{0%{opacity:1;box-shadow:inset 0 0 90px 14px rgba(255,92,80,.5)}100%{opacity:0}}
.cz-danger{position:absolute;inset:0;pointer-events:none;opacity:0;z-index:1}
.cz-danger.on{animation:czdanger 1.05s ease-in-out infinite}
@keyframes czdanger{0%,100%{opacity:0;box-shadow:inset 0 0 100px 16px rgba(255,46,46,0)}50%{opacity:1;box-shadow:inset 0 0 120px 30px rgba(255,46,46,.4)}}
@keyframes killpop{0%{transform:translateX(0) scale(1)}28%{transform:scale(1.35);color:#fff;text-shadow:0 0 16px #fff}100%{transform:scale(1)}}
@media (prefers-reduced-motion:reduce){[data-cz] *{animation-duration:.01ms!important;transition-duration:.01ms!important}}
`;

export function makeUI(canvas) {
  const host = canvas.parentNode; if (getComputedStyle(host).position === 'static') host.style.position = 'relative';
  try { canvas.style.position = canvas.style.position || 'relative'; canvas.style.zIndex = '1'; } catch {}   // keep the canvas under the overlay HUD
  if (!document.getElementById('cz-style')) { const st = document.createElement('style'); st.id = 'cz-style'; st.textContent = CSS; document.head.appendChild(st); }
  const el = document.createElement('div'); el.setAttribute('data-cz', '');
  el.innerHTML = `
    <div data-hub style="display:none">
      <div class="cz-panel cz-top"><span class="cr" data-cr></span><span class="sec" data-sec></span><span class="vit" data-vit></span></div>
      <div class="cz-tabs" data-tabs></div>
      <div class="cz-body" data-body></div>
      <div class="cz-hint" data-hint></div>
    </div>
    <div data-cmb class="cz-cmb" style="display:none">
      <div class="cz-crt"></div>
      <div class="cz-danger" data-danger></div>
      <div class="cz-frame"><i></i><i></i><i></i><i></i></div>
      <div class="cz-wave" data-wave></div>
      <div class="cz-kills">ABBATTUTI <b data-ki>0</b></div>
      <div class="cz-tgt" data-tgt>○ SCANSIONE</div>
      <div class="cz-miss">MISSILI <span data-miss></span></div>
      <div class="cz-gauge sh"><label>SCUDO</label><b data-sh>0</b><span class="seg" data-segsh><i></i></span></div>
      <div class="cz-gauge hu"><label>SCAFO</label><b data-hu>0</b><span class="seg" data-seghu><i></i></span></div>
      <div class="cz-banner" data-ban></div>
    </div>
    <div data-deb class="cz-deb" style="display:none"><div class="t" data-dt></div><div data-dl style="font-size:clamp(15px,3vw,22px);color:#cfd8e6"></div><div class="cz-btn" data-act="Enter">Continua</div></div>
    <div data-modal class="cz-modal" style="display:none"></div>
    <div data-fx class="cz-fx"></div>
    <div data-toast></div>`;
  host.appendChild(el);
  const $ = (s) => el.querySelector(s);
  let lastKey = '', bannerT = 0, lastBan = '', lastFx = null;

  function show(phase, screen) {
    $('[data-hub]').style.display = phase === 'hub' ? 'block' : 'none';
    $('[data-cmb]').style.display = phase === 'combat' ? 'block' : 'none';
    $('[data-deb]').style.display = phase === 'debrief' ? 'flex' : 'none';
    $('[data-modal]').style.display = (phase === 'loading' || phase === 'new_run' || phase === 'conflict') ? 'flex' : 'none';
    el.style.setProperty('--screen', screen || '');
  }
  function paint(m, st) {
    const r = m.run, ph = st.phase;
    if (ph === 'loading') { $('[data-modal]').innerHTML = `<div class="cz-panel cz-card"><h3>Costellazioni 3D</h3><div>Sincronizzazione run…</div><div class="cz-spin"><i></i></div></div>`; return; }
    if (ph === 'new_run') { $('[data-modal]').innerHTML = `<div class="cz-panel cz-card"><h3>Nessuna run da continuare</h3><div style="color:#9fb0c4;max-width:360px">Avvia una nuova run web. Verra' sincronizzata col Cardputer al primo salvataggio.</div><div class="cz-btn cz-row sel" data-row="0">Inizia</div></div>`; return; }
    if (ph === 'conflict') { $('[data-modal]').innerHTML = `<div class="cz-panel cz-card"><h3 style="color:#ffbe40">Run avanzata su Cardputer</h3><div style="color:#9fb0c4;max-width:360px">Ho ricaricato l'ultimo stato dal dispositivo (settore ${r ? r.sector : 0}, ${r ? r.credits : 0} cr).</div><div class="cz-btn cz-row sel" data-row="0">OK</div></div>`; return; }
    if (!r) return;
    // brief full-screen juice (jump warp / sector clear / beacon ignite / trade pop / denial) — runs every frame
    const fx = $('[data-fx]');
    if (st.flash && st.flash.until && (st.clock || 0) < st.flash.until) {
      if (lastFx !== st.flash) { lastFx = st.flash; fx.style.animation = 'none'; void fx.offsetWidth; fx.className = 'cz-fx ' + st.flash.kind; fx.style.animation = ''; }
    } else if (lastFx) { lastFx = null; fx.className = 'cz-fx'; }
    const sc = st.screen, sys = m.sector && m.sector[r.sys];
    const key = [ph, sc, st.focus[sc], st.marketCol, st.marketQty, st.target, r.credits, r.fuel, r.sys, r.sector, r.epoch, r.hull, m.econ.cargoUsed(r), m.econ.beaconsLit(m.CONTENT, r), r.weapon, r.jump_range, r.sensors, r.shield_max, r.hull_max, r.cargo_max, st.toast ? st.toast.text : ''].join('|');
    if (key === lastKey) return; lastKey = key;
    // status strip
    $('[data-cr]').textContent = '◈ ' + r.credits + ' cr';
    const lit = m.econ.beaconsLit(m.CONTENT, r), tot = m.econ.beaconsTotal(m.CONTENT);
    let pips = ''; for (let i = 0; i < tot; i++) pips += `<span class="cz-pip ${i < lit ? 'on' : ''}"></span>`;
    $('[data-sec]').innerHTML = `· SETTORE ${r.sector} · FARI ${pips}`;
    $('[data-vit]').innerHTML = `SCAFO ${r.hull}/${r.hull_max} · SCUDO ${r.shield_max} · CARB ${r.fuel}/${r.fuel_max} · STIVA ${m.econ.cargoUsed(r)}/${r.cargo_max}`;
    // accent theme to current faction
    el.style.setProperty('--acc', sys ? FAC[sys.faction] : '#60cee8');
    // tabs
    const TABS = ['Plancia', 'Mappa', 'Mercato', 'Cantiere', 'Missioni'];
    $('[data-tabs]').innerHTML = TABS.map((t, i) => `<div class="cz-tab ${SCR[i] === sc ? 'on' : ''}" data-tab="${i}">${t}</div>`).join('');
    // body
    $('[data-body]').innerHTML = ({ bridge: bBridge, map: bMap, market: bMarket, shipyard: bShipyard, missions: bMissions }[sc] || bBridge)(m, st, sys);
    $('[data-hint]').innerHTML = HINTS[sc] || HINTS.bridge;
    // toast
    const tEl = $('[data-toast]');
    tEl.innerHTML = st.toast ? `<div class="cz-toast ${st.toast.kind}">${esc(st.toast.text)}</div>` : '';
  }
  function tickVitals(st) {
    $('[data-sh]').textContent = Math.round(st.shield); $('[data-hu]').textContent = Math.max(0, Math.round(st.hull));
    const shp = st.shieldMax ? st.shield / st.shieldMax * 100 : 0, hhp = Math.max(0, st.hull) / (st.hullMax || 100) * 100;
    $('[data-segsh] i').style.width = shp + '%';
    $('[data-seghu] i').style.width = hhp + '%';
    $('.cz-gauge.hu').classList.toggle('low', hhp < 30);
    $('[data-danger]').classList.toggle('on', hhp < 30 && st.hull > 0);   // critical-hull screen vignette
    const ki = $('[data-ki]');                                           // kill counter: punch the chip up on each new kill
    if (ki._k !== st.kills) { if (ki._k != null && st.kills > ki._k) { const p = ki.parentElement; p.style.animation = 'none'; void p.offsetWidth; p.style.animation = 'killpop .42s ease-out'; } ki._k = st.kills; }
    ki.textContent = st.kills;
    const tg = $('[data-tgt]'), on = st.lock >= 0; tg.textContent = on ? '● BERSAGLIO AGGANCIATO' : '○ SCANSIONE…'; tg.classList.toggle('lock', on);
    const mn = st.missiles != null ? st.missiles : 0; let mh = ''; for (let i = 0; i < 5; i++) mh += i < mn ? '◆' : '◇'; $('[data-miss]').textContent = mh;
    const wv = $('[data-wave]'), total = (st.cc && st.cc.waves) || st.wave || 0;
    if (wv._n !== total) { wv._n = total; let h = ''; for (let i = 0; i < total; i++) h += '<s></s>'; wv.innerHTML = h; }
    const kids = wv.children; for (let i = 0; i < kids.length; i++) kids[i].classList.toggle('on', i < st.wave);
  }
  function combat(st) {
    tickVitals(st);
    if (st.banner && st.banner !== lastBan && st.clock < st.bannerUntil) { lastBan = st.banner; const b = $('[data-ban]'); b.textContent = st.banner; b.style.opacity = 1; clearTimeout(bannerT); bannerT = setTimeout(() => { b.style.opacity = 0; lastBan = ''; }, 1300); }
  }
  function debrief(st) { const win = st.result === 1; $('[data-dt]').textContent = win ? 'VITTORIA' : 'RITIRATA'; $('[data-dt]').style.color = win ? '#60cee8' : '#ff5c50'; $('[data-dl]').textContent = `Abbattimenti ${st.dbKills} · +${st.earnCr || 0} cr`; }

  // mouse -> synthetic keydown (single input path with the keyboard)
  const fire = (k) => window.dispatchEvent(new KeyboardEvent('keydown', { key: k, bubbles: true }));
  function onClick(e) {
    const tab = e.target.closest('[data-tab]'); if (tab) { fire('Tab:' + tab.dataset.tab); return; }
    const tgt = e.target.closest('[data-tgt]'); if (tgt) { fire('Tgt:' + tgt.dataset.tgt); return; }   // select destination; jump via the action row
    const act = e.target.closest('[data-act]'); if (act) { fire(act.dataset.act); return; }            // specific affordance (market Buy/Sell cell, debrief Continua) — BEFORE the row default
    const row = e.target.closest('[data-row]'); if (row) { const sc = el.style.getPropertyValue('--screen').trim() || 'bridge'; fire('Hover:' + sc + ':' + row.dataset.row); fire('Enter'); return; }
  }
  function onOver(e) { const row = e.target.closest('[data-row]'); if (row) { const sc = el.style.getPropertyValue('--screen').trim() || 'bridge'; fire('Hover:' + sc + ':' + row.dataset.row); } }
  function onCtx(e) { e.preventDefault(); fire('Escape'); }
  el.addEventListener('click', onClick); el.addEventListener('mouseover', onOver); el.addEventListener('contextmenu', onCtx);
  el.addEventListener('pointerdown', (e) => e.stopPropagation(), true);   // menu clicks never reach the canvas (no pointer-lock)

  return { el, show, paint, tickVitals, combat, debrief, setTheme() {}, dispose() { try { el.remove(); canvas.style.zIndex = ''; canvas.style.position = ''; } catch {} } };   // un-promote the shared canvas for the next game
}

const SCR = ['bridge', 'map', 'market', 'shipyard', 'missions'];
const HINTS = {
  bridge: `<span><kbd>↑↓</kbd> scegli</span><span><kbd>Invio</kbd>/<kbd>A</kbd> apri</span><span><kbd>1-5</kbd> sezioni</span>`,
  map: `<span><kbd>←→</kbd> bersaglio</span><span><kbd>↑↓</kbd> azione</span><span><kbd>Invio</kbd>/<kbd>A</kbd> salta/accendi</span><span><kbd>Esc</kbd>/<kbd>B</kbd> indietro</span>`,
  market: `<span><kbd>↑↓</kbd> bene</span><span><kbd>←→</kbd>/<kbd>X</kbd> compra/vendi</span><span><kbd>,.</kbd> qta'</span><span><kbd>R</kbd>/<kbd>Y</kbd> carburante</span><span><kbd>Invio</kbd>/<kbd>A</kbd> ok</span>`,
  shipyard: `<span><kbd>↑↓</kbd> scegli</span><span><kbd>Invio</kbd>/<kbd>A</kbd> compra</span><span><kbd>Esc</kbd> indietro</span>`,
  missions: `<span><kbd>↑↓</kbd> scegli</span><span><kbd>Invio</kbd>/<kbd>A</kbd> lancia</span><span><kbd>Esc</kbd>/<kbd>B</kbd> plancia</span>`,
};

// ---- screen builders (return HTML for [data-body]) ---------------------------------------------
function repBars(m, r) {
  return `<div class="cz-rep">` + m.CONTENT.factions.map((f, i) => { const v = r.rep[i]; const w = Math.abs(v) / 2; const col = v >= 0 ? '#76e68c' : '#ff5c50'; return `<span style="color:${FAC[i]}">${esc(f.it)}</span><span class="b"><i style="${v >= 0 ? 'left:50%' : 'right:50%'};width:${w}%;background:${col}"></i></span><span style="color:#9fb0c4">${v}</span>`; }).join('') + `</div>`;
}
function bBridge(m, st, sys) {
  const r = m.run, dots = (n) => { let s = ''; for (let i = 0; i < 4; i++) s += `<s class="${i < n ? 'on' : ''}"></s>`; return `<span class="cz-pips">${s}</span>`; };
  const acts = [['Mappa stellare', 'salta tra i sistemi'], ['Mercato', 'compra e vendi merci'], ['Cantiere', 'potenzia la nave'], [`Missioni (${m.missions ? m.missions.length : 0})`, 'lancia un dogfight'], ['Parti', 'al volo: pattuglia']];
  const list = acts.map((a, i) => `<div class="cz-row ${st.focus.bridge === i ? 'sel' : ''}" data-row="${i}"><b>${esc(a[0])}</b><span class="cz-sub" style="margin:0 0 0 auto">${esc(a[1])}</span></div>`).join('');
  return `<div class="cz-col" style="flex:1.1"><div class="cz-panel cz-card"><h3>Plancia · ${sys ? esc(sys.it) : ''}</h3>
    <div style="display:flex;gap:8px;margin-bottom:10px"><span class="cz-chip" style="background:${FAC[sys ? sys.faction : 0]}33;color:${FAC[sys ? sys.faction : 0]}">${sys ? esc(m.CONTENT.factions[sys.faction].it) : ''}</span><span class="cz-chip" style="background:${ECO[sys ? sys.econ : 0]}22;color:${ECO[sys ? sys.econ : 0]}">${sys ? esc(m.CONTENT.econ[sys.econ].it) : ''}</span></div>
    <div style="display:grid;grid-template-columns:1fr 1fr;gap:8px 18px;font-size:13px;color:#cfd8e6">
      <span>Crediti <b style="color:#ffbe40">${r.credits}</b></span><span>Scafo <b>${r.hull}/${r.hull_max}</b></span>
      <span>Scudo <b style="color:#60cee8">${r.shield_max}</b></span><span>Laser ${dots(r.weapon)}</span>
      <span>Salto <b>${r.jump_range}</b></span><span>Sensori ${dots(r.sensors)}</span></div>
    <h3 style="margin-top:14px">Reputazione</h3>${repBars(m, r)}</div></div>
   <div class="cz-col" style="flex:1"><div class="cz-panel cz-card" style="flex:1"><h3>Comando</h3><div class="cz-list">${list}</div></div></div>`;
}
function bMap(m, st, sys) {
  const r = m.run, S = m.sector;
  let nodes = '';
  for (let i = 0; i < S.length; i++) {
    const s = S[i], cur = i === r.sys, tg = i === st.target;
    const lit = (r.beacon_lit >>> 0) & (1 << i);
    const inR = m.econ.sysDist(m.CONTENT, r.sys, i) <= r.jump_range;
    const bk = s.beacon ? `<span class="bk" style="border-color:${lit ? '#ffbe40' : '#ff5c50'};border-style:${lit ? 'solid' : 'dashed'}"></span>` : '';
    nodes += `<div class="cz-node ${cur ? 'cur' : ''} ${tg ? 'tgt' : ''}" data-tgt="${i}" style="left:${s.x}%;top:${s.y}%;background:${FAC[s.faction]};opacity:${inR || cur ? 1 : .45}">${bk}${(cur || tg) ? `<span class="lbl">${esc(s.it)}</span>` : ''}</div>`;
  }
  const t = st.target, ts = S[t] || sys, d = m.econ.sysDist(m.CONTENT, r.sys, t), cost = m.econ.jumpCost(d), inR = d <= r.jump_range, aff = r.fuel >= cost;
  const acts = ['jump']; const showRel = sys && sys.beacon && !((r.beacon_lit >>> 0) & (1 << r.sys)); if (showRel) acts.push('relight'); acts.push('back');
  const labels = { jump: [`Salta`, inR && aff ? `${cost} celle · dist ${Math.round(d)}` : (!inR ? 'fuori portata' : 'celle insuff.')], relight: ['Accendi Faro', '-300 cr · -1 Reliquia'], back: ['Plancia', ''] };
  const list = acts.map((a, i) => `<div class="cz-row ${st.focus.map === i ? 'sel' : ''} ${a === 'jump' && (!inR || !aff) ? 'dis' : ''}" data-row="${i}"><b>${labels[a][0]}</b><span class="cz-sub" style="margin:2px 0 0 auto">${labels[a][1]}</span></div>`).join('');
  return `<div class="cz-panel cz-map" style="padding:8px">${nodes}</div>
   <div class="cz-col" style="flex:0 0 250px"><div class="cz-panel cz-card"><h3>${ts ? esc(ts.it) : ''}</h3>
     <div style="display:flex;gap:6px;margin-bottom:8px"><span class="cz-chip" style="color:${FAC[ts ? ts.faction : 0]}">${ts ? esc(m.CONTENT.factions[ts.faction].it) : ''}</span><span class="cz-chip" style="color:${ECO[ts ? ts.econ : 0]}">${ts ? esc(m.CONTENT.econ[ts.econ].it) : ''}</span>${ts && ts.beacon ? `<span class="cz-chip" style="color:#ffbe40">Faro</span>` : ''}</div>
     <div class="cz-list">${list}</div></div></div>`;
}
function bMarket(m, st, sys) {
  const r = m.run, used = m.econ.cargoUsed(r);
  let rows = '';
  for (let g = 0; g < m.CONTENT.goods.length; g++) {
    const good = m.CONTENT.goods[g], buy = m.econ.unitBuy(m.CONTENT, r.sys, g, r.epoch, r.rep), sell = m.econ.unitSell(m.CONTENT, r.sys, g, r.epoch, r.rep);
    const canBuy = r.credits >= buy && used < r.cargo_max, sel = st.focus.market === g;
    rows += `<div class="cz-row ${sel ? 'sel' : ''}" data-row="${g}"><b>${esc(good.it)}</b>
      <span style="margin-left:auto;display:flex;gap:10px;align-items:center;font-weight:700">
        <span class="cz-cell ${sel && !st.marketCol ? 'on' : ''}" data-act="Buy:${g}" style="color:${canBuy ? '#ffbe40' : '#7c899b'}">C ${buy}</span>
        <span class="cz-cell ${sel && st.marketCol ? 'on' : ''}" data-act="Sell:${g}" style="color:${r.cargo[g] > 0 ? '#76e68c' : '#7c899b'}">V ${sell}</span>
        <span style="color:${r.cargo[g] ? '#ffbe40' : '#7c899b'};min-width:34px;text-align:right">×${r.cargo[g]}</span></span></div>`;
  }
  const fg = m.CONTENT.goods.length, price = m.econ.refuelPrice(m.CONTENT, r.sys);
  rows += `<div class="cz-row ${st.focus.market === fg ? 'sel' : ''}" data-row="${fg}"><b>Carburante</b><span style="margin-left:auto;font-weight:700;color:#60cee8">${price} cr/cella</span><span style="margin-left:14px;color:#ffbe40">${r.fuel}/${r.fuel_max}</span></div>`;
  return `<div class="cz-panel cz-card" style="flex:1"><h3>Mercato · ${sys ? esc(sys.it) : ''} <span style="float:right;color:#9fb0c4;font-weight:700">${st.marketCol ? 'VENDI' : 'COMPRA'} · qta' ${st.marketQty} · stiva ${used}/${r.cargo_max}</span></h3><div class="cz-list">${rows}</div></div>`;
}
function bShipyard(m, st) {
  const r = m.run; let rows = '';
  const dots = (n) => { let s = ''; for (let i = 0; i < 4; i++) s += `<s class="${i < n ? 'on' : ''}"></s>`; return `<span class="cz-pips">${s}</span>`; };
  for (let i = 0; i < m.shop.SHOP.length; i++) {
    const it = m.shop.SHOP[i], maxed = m.shop.shopMaxed(it.key, r), cost = m.shop.shopCost(it.key, r), lv = Math.max(0, Math.floor(it.level(r))), aff = !maxed && r.credits >= cost;
    rows += `<div class="cz-row ${st.focus.shipyard === i ? 'sel' : ''} ${maxed ? 'dis' : ''}" data-row="${i}"><b>${esc(it.label[0])}</b><span style="margin:0 8px 0 14px">${dots(lv)}</span><span class="r" style="color:${maxed ? '#7c899b' : (aff ? '#ffbe40' : '#ff5c50')}">${maxed ? 'MAX' : cost + ' cr'}</span></div>`;
  }
  const ri = m.shop.SHOP.length, rc = m.shop.repairCost(r), intact = r.hull >= r.hull_max;
  rows += `<div class="cz-row ${st.focus.shipyard === ri ? 'sel' : ''} ${intact ? 'dis' : ''}" data-row="${ri}"><b>Riparazione</b><span class="cz-sub" style="margin:2px 0 0 14px">${r.hull}/${r.hull_max}</span><span class="r" style="color:${intact ? '#7c899b' : (r.credits >= rc ? '#ffbe40' : '#ff5c50')}">${intact ? 'integro' : rc + ' cr'}</span></div>`;
  return `<div class="cz-panel cz-card" style="flex:1"><h3>Cantiere · ${r.credits} cr</h3><div class="cz-list">${rows}</div></div>`;
}
function bMissions(m, st, sys) {
  const ms = m.missions || []; let rows = '';
  for (let i = 0; i < ms.length; i++) {
    const mi = ms[i], fl = mi.flavor || {};
    const mods = (fl.mods || []).map((x) => `<span class="cz-chip" style="font-size:10px;background:rgba(255,190,64,.12);color:#ffbe40">${esc(x)}</span>`).join(' ');
    rows += `<div class="cz-row ${st.focus.missions === i ? 'sel' : ''}" data-row="${i}"><div style="min-width:0;flex:1">`
      + `<b style="color:${fl.color || '#e6edf3'}">${fl.star ? esc(fl.star) + ' ' : ''}${esc(fl.title || (MT[mi.type] ? MT[mi.type][0] : ''))}${sys ? ' · ' + esc(sys.it) : ''}</b>`
      + `<div class="cz-sub" style="color:#aeb9c8">${esc(fl.brief || '')}</div>`
      + `<div class="cz-sub" style="margin-top:3px">${mi.waves}×${mi.per_wave} ostili${mi.ace ? ' · ASSO' : ''} · vs <span style="color:${FAC[mi.foe_fac]}">${esc(m.CONTENT.factions[mi.foe_fac].it)}</span> ${mods}</div>`
      + `</div><span class="r" style="margin-left:auto;text-align:right"><span style="font-size:10px;display:block;color:${fl.color || '#9fb0c4'};opacity:.85">${esc(fl.rarityName || '')}</span><span style="color:#ffbe40">${mi.reward_cr} cr</span></span></div>`;
  }
  rows += `<div class="cz-row ${st.focus.missions === ms.length ? 'sel' : ''}" data-row="${ms.length}"><div><b>Pattuglia libera</b><div class="cz-sub">spazza i predoni · solo salvataggio</div></div><span class="r" style="color:#9fb0c4;margin-left:auto">salvataggio</span></div>`;
  return `<div class="cz-panel cz-card" style="flex:1"><h3>Sala Missioni · ${sys ? esc(sys.it) : ''}</h3><div class="cz-list">${rows}</div></div>`;
}
