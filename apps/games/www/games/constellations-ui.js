// constellations-ui.js — the DOM/CSS hub of Costellazioni over the live 3D backdrop: status strip,
// tabs, the five hub screens (Bridge / Map / Market / Shipyard / Missions), toasts, the load / new-run
// / conflict modals and the after-action report. Every string comes from the games catalog (cz_*
// keys, five languages); the mission flavor from the shared generator is shown through its index
// form (archetype, gang, epithet, modifiers) so it reads in the OS language too. Mouse works by
// synthesizing the keydown events the harness already forwards (click == keyboard: one input path).
// Diff-gated repaint: the DOM is rebuilt only when a state key changes. Illustrations (title art,
// faction halls and emblems) stream from stelle/assets.js when present, with gradient placeholders.
import I18N from '/nucleo-i18n.js';
import * as A from '/apps/games/games/stelle/assets.js';
import { ACE_NAMES } from '/apps/games/games/stelle/sim.js';

const t = I18N.scope('games');
const tr = (k, v) => { const s = t(k, v); return s === k ? '' : s; };
const FAC = ['#7fa8f0', '#3fbfa6', '#e0773c', '#a882e6'];
const ECO = ['#7fc77a', '#c9a25a', '#8aa0b8', '#9b8cff', '#5ad0c0'];
const HALL = ['hall_gilda', 'hall_custodi', 'hall_relitti', 'hall_eco'];
const EMBLEM = ['emblem_gilda', 'emblem_custodi', 'emblem_relitti', 'emblem_eco'];
const RARC = ['#9fb0bf', '#5ee6ff', '#b46be0', '#e0b13b'];
const SHOPK = { hull: 'cz_up_hull', shield: 'cz_up_shield', weapon: 'cz_up_weapon', cargo: 'cz_up_cargo', jump: 'cz_up_jump', sensors: 'cz_up_sensors', tank: 'cz_up_tank' };
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

const CSS = `
[data-cz]{position:absolute;inset:0;pointer-events:none;z-index:30;font-family:'Segoe UI',system-ui,sans-serif;color:#e6edf3;font-variant-numeric:tabular-nums;
 --acc:#60cee8;--accg:rgba(96,206,232,.55);--glass:rgba(8,14,26,.66);--stroke:rgba(150,190,230,.16);--edge:#25324a;--hull:rgba(12,20,38,.82);--good:#76e68c;--warn:#ffbe40;--bad:#ff5c50;--lo:#9fb0c4;--dim:#7c899b}
[data-cz] *{box-sizing:border-box}
.cz-panel{background:var(--glass);border:1px solid var(--stroke);border-radius:14px;backdrop-filter:blur(10px) saturate(1.15);box-shadow:0 18px 50px rgba(0,0,0,.45),inset 0 1px 0 rgba(255,255,255,.05)}
@supports not (backdrop-filter:blur(2px)){.cz-panel{background:rgba(10,18,34,.92)}}
.cz-top{position:absolute;top:14px;left:14px;right:62px;height:46px;display:flex;align-items:center;gap:16px;padding:0 18px;border-radius:999px;font-size:13px;font-weight:700}
.cz-top .cr{color:var(--warn);font-size:15px} .cz-top .sec{color:var(--lo)} .cz-pip{display:inline-block;width:8px;height:8px;border-radius:50%;margin:0 2px;border:1px solid var(--acc)} .cz-pip.on{background:var(--acc);box-shadow:0 0 8px var(--accg)}
.cz-top .vit{margin-left:auto;display:flex;gap:14px;color:var(--lo);font-size:12px}
.cz-tabs{position:absolute;top:70px;left:14px;right:14px;display:flex;gap:8px;pointer-events:auto;flex-wrap:wrap}
.cz-tab{flex:0 0 auto;padding:8px 16px;border-radius:999px;border:1px solid var(--edge);background:var(--hull);color:var(--lo);font-weight:700;font-size:13px;cursor:pointer;transition:.14s}
.cz-tab:hover{border-color:#3b7e90}.cz-tab.on{color:#06121c;background:var(--acc);border-color:var(--acc);box-shadow:0 0 18px var(--accg)}
.cz-body{position:absolute;top:116px;left:14px;right:14px;bottom:50px;display:flex;gap:14px;animation:sweep .18s cubic-bezier(.22,.61,.36,1)}
@keyframes sweep{from{opacity:0;transform:translateY(10px)}to{opacity:1;transform:none}}
.cz-col{display:flex;flex-direction:column;gap:12px;min-width:0}
.cz-card{padding:16px;flex:0 0 auto}.cz-card h3{margin:0 0 10px;font-size:13px;letter-spacing:.08em;color:var(--lo);text-transform:uppercase;font-weight:800}
.cz-hero{position:relative;height:120px;margin:-16px -16px 14px;border-radius:14px 14px 0 0;overflow:hidden;background-size:cover;background-position:center}
.cz-hero::after{content:'';position:absolute;inset:0;background:linear-gradient(180deg,transparent 30%,rgba(8,14,26,.95))}
.cz-hero .em{position:absolute;left:14px;bottom:10px;width:44px;height:44px;border-radius:8px;background-size:cover;background-position:center;z-index:1;border:1px solid rgba(255,255,255,.2)}
.cz-hero b{position:absolute;left:68px;bottom:16px;z-index:1;font-size:20px;letter-spacing:.02em;text-shadow:0 2px 10px #000}
.cz-list{display:flex;flex-direction:column;gap:8px;overflow-y:auto;overflow-x:hidden;pointer-events:auto;padding:2px 4px 2px 2px}
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
.cz-pips{display:inline-flex;gap:3px}.cz-pips s{width:10px;height:5px;border-radius:2px;background:#16203a}.cz-pips s.on{background:var(--good)}
.cz-rep{display:grid;grid-template-columns:auto 1fr auto;gap:6px 8px;align-items:center;font-size:12px}
.cz-rep .b{height:7px;border-radius:4px;background:#16203a;position:relative}.cz-rep .b i{position:absolute;top:0;bottom:0;left:50%;background:var(--good)}
.cz-hint{position:absolute;left:14px;right:14px;bottom:10px;height:32px;display:flex;align-items:center;justify-content:center;gap:18px;font-size:12px;color:var(--lo);flex-wrap:wrap}
.cz-hint kbd{background:#10203a;border:1px solid var(--edge);border-radius:6px;padding:1px 7px;font-family:inherit;color:#cfd8e6;margin:0 3px}
.cz-toast{position:absolute;left:50%;transform:translateX(-50%);bottom:54px;padding:8px 18px;border-radius:999px;font-weight:700;font-size:14px;pointer-events:none;animation:sweep .18s}
.cz-toast.bad{background:rgba(60,16,16,.85);color:var(--bad);border:1px solid var(--bad)}.cz-toast.good{background:rgba(16,48,28,.85);color:var(--good);border:1px solid var(--good)}.cz-toast.warn{background:rgba(60,46,12,.85);color:var(--warn);border:1px solid var(--warn)}
.cz-map{position:relative;flex:1 1 auto;aspect-ratio:1/1;max-height:100%;align-self:center}
.cz-node{position:absolute;width:14px;height:14px;border-radius:50%;transform:translate(-50%,-50%);border:1px solid #05060f;cursor:pointer;pointer-events:auto}
.cz-node.cur{box-shadow:0 0 0 4px rgba(118,230,140,.25);animation:pulse 1.4s infinite}
.cz-node.tgt{box-shadow:0 0 0 3px var(--acc),0 0 16px var(--accg)}
.cz-node .bk{position:absolute;inset:-5px;border-radius:50%;border:2px solid}
.cz-node .lbl{position:absolute;left:50%;top:16px;transform:translateX(-50%);white-space:nowrap;font-size:10px;color:var(--lo);text-shadow:0 0 6px #000}
.cz-link{position:absolute;height:1px;transform-origin:0 50%;background:linear-gradient(90deg,rgba(96,206,232,.0),rgba(96,206,232,.35),rgba(96,206,232,.0));pointer-events:none}
@keyframes pulse{0%,100%{box-shadow:0 0 0 4px rgba(118,230,140,.28)}50%{box-shadow:0 0 0 8px rgba(118,230,140,.08)}}
.cz-modal{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;flex-direction:column;text-align:center;gap:14px;background-size:cover;background-position:center}
.cz-modal .cz-card{max-width:460px;align-items:center;display:flex;flex-direction:column;gap:12px}
.cz-title{font-size:clamp(34px,6vw,64px);font-weight:900;letter-spacing:.12em;text-transform:uppercase;color:#fff;text-shadow:0 0 30px rgba(120,200,255,.6),0 4px 20px #000}
.cz-intros{display:flex;flex-direction:column;gap:6px;margin:4px 0 8px;font-size:clamp(14px,1.6vw,18px);letter-spacing:.04em;color:#dfe9f5;text-shadow:0 2px 12px #000}
.cz-intro{opacity:0;animation:czin 1.2s ease-out forwards}.cz-intro:last-child{color:#ffd66b}
@keyframes czin{from{opacity:0;transform:translateY(6px)}to{opacity:1;transform:none}}
.cz-spin{width:160px;height:6px;border-radius:3px;background:#16203a;overflow:hidden}.cz-spin i{display:block;width:40%;height:100%;background:var(--acc);animation:slide 1.1s infinite}
@keyframes slide{from{margin-left:-40%}to{margin-left:100%}}
[data-hub]{cursor:default}
.cz-deb{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;flex-direction:column;text-align:center;gap:10px;background:radial-gradient(ellipse at 50% 50%,rgba(4,8,16,.35),rgba(4,8,16,.75))}
.cz-deb .t{font-weight:900;font-size:clamp(30px,7vw,60px);letter-spacing:.14em;text-transform:uppercase;text-shadow:0 0 30px currentColor}
.cz-deb .s{font-size:13px;letter-spacing:.3em;text-transform:uppercase;color:var(--lo)}
.cz-stats{display:grid;grid-template-columns:repeat(3,minmax(90px,1fr));gap:10px;margin:10px 0;min-width:min(520px,90vw)}
.cz-stats div{padding:10px;border:1px solid var(--stroke);border-radius:10px;background:var(--glass)}
.cz-stats b{display:block;font-size:22px;color:#fff}.cz-stats span{font-size:11px;letter-spacing:.12em;text-transform:uppercase;color:var(--lo)}
.cz-btn{padding:10px 22px;border-radius:12px;border:1px solid var(--acc);background:rgba(40,70,120,.4);color:#e6edf3;font-weight:800;font-size:15px;pointer-events:auto;cursor:pointer}
.cz-fx{position:absolute;inset:0;pointer-events:none;opacity:0;border-radius:18px}
.cz-fx.pop{animation:fxpop .4s ease-out}@keyframes fxpop{0%{opacity:1;background:radial-gradient(circle at 50% 62%,rgba(118,230,140,.22),transparent 58%)}100%{opacity:0}}
.cz-fx.warp{animation:fxwarp .6s ease-out}@keyframes fxwarp{0%{opacity:1;box-shadow:inset 0 0 140px 36px var(--accg)}100%{opacity:0;box-shadow:inset 0 0 0 0 transparent}}
.cz-fx.ignite{animation:fxign .8s ease-out}@keyframes fxign{0%{opacity:1;background:radial-gradient(circle at 50% 50%,rgba(255,190,64,.28),transparent 55%)}100%{opacity:0}}
.cz-fx.sector{animation:fxsec 1.5s ease-out}@keyframes fxsec{0%{opacity:1;background:radial-gradient(circle,rgba(96,206,232,.42),transparent 62%)}45%{opacity:.55}100%{opacity:0}}
.cz-fx.shake{animation:fxbad .26s ease-out}@keyframes fxbad{0%{opacity:1;box-shadow:inset 0 0 90px 14px rgba(255,92,80,.5)}100%{opacity:0}}
@media (max-width:720px){.cz-top .vit{display:none}.cz-body{flex-direction:column;overflow:auto}}
@media (prefers-reduced-motion:reduce){[data-cz] *{animation-duration:.01ms!important;transition-duration:.01ms!important}}
`;

// the recurring cast (tools/costellazioni-assets/lore.md): callsigns are proper names in every language
export { ACE_NAMES };
export const ACE_PORTRAIT = ['ace_lancer', 'ace_vigil', 'ace_gutter', null, 'ace_bram'];
export const GIVER_PORTRAIT = ['broker', 'abbess', 'matriarch', 'echo'];
// localized faction / economy / good names (the CONTENT tables carry it/en only)
const facName = (i) => tr('cz_fac_' + i);
const econName = (i) => tr('cz_econ_' + i);
const goodName = (i) => tr('cz_good_' + i);
// mission flavor in the OS language, from the generator's index form (falls back to its Italian text)
export function flavorText(mi) {
  const fl = mi.flavor || {};
  if (fl.archK == null) return { title: fl.title || '', brief: fl.brief || '', name: fl.name || '' };
  const name = fl.aceI >= 0 ? ACE_NAMES[fl.aceI] : (fl.nameBase || fl.name || '');
  const gang = tr('cz_gang_' + fl.gangI);
  return { title: tr('cz_fa_title_' + fl.archK, { name }), brief: tr('cz_fa_brief_' + fl.archK, { name, gang }), name, gang,
    mods: (fl.modI || []).map((i) => tr('cz_mod_' + i)), rarity: tr('cz_rar_' + (fl.rarity | 0)) };
}

export function makeUI(canvas) {
  const host = canvas.parentNode; if (getComputedStyle(host).position === 'static') host.style.position = 'relative';
  try { canvas.style.position = canvas.style.position || 'relative'; canvas.style.zIndex = '1'; } catch {}
  if (!document.getElementById('cz-style')) { const st = document.createElement('style'); st.id = 'cz-style'; st.textContent = CSS; document.head.appendChild(st); }
  else document.getElementById('cz-style').textContent = CSS;
  const el = document.createElement('div'); el.setAttribute('data-cz', '');
  el.innerHTML = `
    <div data-hub style="display:none">
      <div class="cz-panel cz-top"><span class="cr" data-cr></span><span class="sec" data-sec></span><span class="vit" data-vit></span></div>
      <div class="cz-tabs" data-tabs></div>
      <div class="cz-body" data-body></div>
      <div class="cz-hint" data-hint></div>
    </div>
    <div data-deb class="cz-deb" style="display:none"></div>
    <div data-modal class="cz-modal" style="display:none"></div>
    <div data-fx class="cz-fx"></div>
    <div data-toast></div>`;
  host.appendChild(el);
  const $ = (s) => el.querySelector(s);
  let lastKey = '', lastFx = null, lastDeb = '', lastModal = '';
  A.loadManifest().then(() => { lastKey = ''; lastModal = ''; }).catch(() => {});
  const img = (id, node, css) => {   // stream an illustration into a node's background, placeholder first
    if (!A.has('images', id)) return;
    node.style.backgroundImage = A.placeholder(id);
    A.imageUrl(id).then((u) => { node.style.backgroundImage = (css ? css + ',' : '') + `url("${u}")`; }).catch(() => {});
  };
  I18N.onChange(() => { lastKey = ''; lastDeb = ''; lastModal = ''; });

  function show(phase, screen) {
    $('[data-hub]').style.display = phase === 'hub' ? 'block' : 'none';
    $('[data-deb]').style.display = phase === 'debrief' ? 'flex' : 'none';
    $('[data-modal]').style.display = (phase === 'loading' || phase === 'new_run' || phase === 'conflict') ? 'flex' : 'none';
    el.style.setProperty('--screen', screen || '');
  }
  function modal(html, key) {
    if (key === lastModal) return; lastModal = key;
    const m = $('[data-modal]'); m.innerHTML = html;
    m.style.backgroundImage = 'radial-gradient(ellipse at 50% 40%, rgba(6,10,20,.1), rgba(6,10,20,.8))';
    img('title', m, 'radial-gradient(ellipse at 50% 40%, rgba(6,10,20,.05), rgba(6,10,20,.75))');
  }
  function paint(m, st) {
    const r = m.run, ph = st.phase;
    const title = `<div class="cz-title">${esc(tr('cz_title'))}</div>`;
    if (ph === 'loading') { modal(`${title}<div class="cz-panel cz-card"><div>${esc(tr('cz_ui_syncing'))}</div><div class="cz-spin"><i></i></div></div>`, 'loading'); return; }
    if (ph === 'new_run') {
      const intro = [0, 1, 2, 3, 4].map((i) => `<div class="cz-intro" style="animation-delay:${0.4 + i * 0.7}s">${esc(tr('cz_intro_' + i))}</div>`).join('');
      modal(`${title}<div class="cz-intros">${intro}</div><div class="cz-panel cz-card"><h3>${esc(tr('cz_ui_no_run'))}</h3><div style="color:#9fb0c4;max-width:380px">${esc(tr('cz_ui_new_run_body'))}</div><div class="cz-btn cz-row sel" data-row="0">${esc(tr('cz_ui_start'))}</div></div>`, 'new'); return;
    }
    if (ph === 'conflict') { modal(`${title}<div class="cz-panel cz-card"><h3 style="color:#ffbe40">${esc(tr('cz_ui_conflict_title'))}</h3><div style="color:#9fb0c4;max-width:380px">${esc(tr('cz_ui_conflict_body', { sector: r ? r.sector : 0, cr: r ? r.credits : 0 }))}</div><div class="cz-btn cz-row sel" data-row="0">OK</div></div>`, 'conflict'); return; }
    lastModal = '';
    if (!r) return;
    const fx = $('[data-fx]');
    if (st.flash && st.flash.until && (st.clock || 0) < st.flash.until) {
      if (lastFx !== st.flash) { lastFx = st.flash; fx.style.animation = 'none'; void fx.offsetWidth; fx.className = 'cz-fx ' + st.flash.kind; fx.style.animation = ''; }
    } else if (lastFx) { lastFx = null; fx.className = 'cz-fx'; }
    const sc = st.screen, sys = m.sector && m.sector[r.sys];
    const key = [ph, sc, st.focus[sc], st.marketCol, st.marketQty, st.target, r.credits, r.fuel, r.sys, r.sector, r.epoch, r.hull, m.econ.cargoUsed(r), m.econ.beaconsLit(m.CONTENT, r), r.weapon, r.jump_range, r.sensors, r.shield_max, r.hull_max, r.cargo_max, st.toast ? st.toast.key : '', I18N.lang].join('|');
    if (key === lastKey) return; lastKey = key;
    $('[data-cr]').textContent = '◈ ' + r.credits + ' cr';
    const lit = m.econ.beaconsLit(m.CONTENT, r), tot = m.econ.beaconsTotal(m.CONTENT);
    let pips = ''; for (let i = 0; i < tot; i++) pips += `<span class="cz-pip ${i < lit ? 'on' : ''}"></span>`;
    $('[data-sec]').innerHTML = `· ${esc(tr('cz_ui_sector', { n: r.sector }))} · ${esc(tr('cz_ui_beacons'))} ${pips}`;
    $('[data-vit]').textContent = tr('cz_ui_vitals', { h: r.hull, hm: r.hull_max, s: r.shield_max, f: r.fuel, fm: r.fuel_max, c: m.econ.cargoUsed(r), cm: r.cargo_max });
    el.style.setProperty('--acc', sys ? FAC[sys.faction] : '#60cee8');
    const TABS = ['cz_tab_bridge', 'cz_tab_map', 'cz_tab_market', 'cz_tab_shipyard', 'cz_tab_missions'];
    $('[data-tabs]').innerHTML = TABS.map((k, i) => `<div class="cz-tab ${SCR[i] === sc ? 'on' : ''}" data-tab="${i}">${esc(tr(k))}</div>`).join('');
    $('[data-body]').innerHTML = ({ bridge: bBridge, map: bMap, market: bMarket, shipyard: bShipyard, missions: bMissions }[sc] || bBridge)(m, st, sys);
    if (sc === 'bridge' && sys) { const hero = $('.cz-hero'); if (hero) { img(HALL[sys.faction], hero); const em = hero.querySelector('.em'); if (em) img(EMBLEM[sys.faction], em); } }
    $('[data-hint]').innerHTML = hints(sc);
    const tEl = $('[data-toast]');
    tEl.innerHTML = st.toast ? `<div class="cz-toast ${st.toast.kind}">${esc(st.toast.key ? tr(st.toast.key, st.toast.v) : (st.toast.text || ''))}</div>` : '';
  }
  function debrief(st, F) {
    const k = [st.result, st.earnCr, st.dbKills, I18N.lang].join('|');
    if (k === lastDeb) return; lastDeb = k;
    const win = st.result === 1, dead = st.result === 2, s = st.dbStats || {};
    const title = tr(win ? 'cz_deb_victory' : dead ? 'cz_deb_destroyed' : 'cz_deb_retreat');
    const col = win ? '#ffd66b' : dead ? '#ff5c50' : '#9fd8ff';
    const mm = Math.floor((s.time || 0) / 60), ss = String((s.time || 0) % 60).padStart(2, '0');
    const extra = [];
    if (s.aceKill) extra.push(tr('cz_deb_ace')); if (s.capKill) extra.push(tr('cz_deb_capital'));
    $('[data-deb]').innerHTML = `<div class="s">${esc(tr('cz_mk_' + (s.kind || 'patrol')))}</div><div class="t" style="color:${col}">${esc(title)}</div>
      <div class="cz-stats">
        <div><b>${st.dbKills | 0}</b><span>${esc(tr('cz_deb_kills'))}</span></div>
        <div><b style="color:#ffd66b">+${st.earnCr || 0}</b><span>${esc(tr('cz_deb_credits'))}</span></div>
        <div><b>${mm}:${ss}</b><span>${esc(tr('cz_deb_time'))}</span></div>
        <div><b>${s.acc || 0}%</b><span>${esc(tr('cz_deb_accuracy'))}</span></div>
        <div><b>${s.hits || 0}/${s.shots || 0}</b><span>${esc(tr('cz_deb_hits'))}</span></div>
        <div><b>${s.dmgIn || 0}</b><span>${esc(tr('cz_deb_damage'))}</span></div>
      </div>
      ${extra.length ? `<div style="color:#ffd66b;font-weight:700">${esc(extra.join(' · '))}</div>` : ''}
      <div class="cz-btn" data-act="Enter">${esc(tr('cz_deb_continue'))}</div>`;
  }

  const fire = (k) => window.dispatchEvent(new KeyboardEvent('keydown', { key: k, bubbles: true }));
  function onClick(e) {
    const tab = e.target.closest('[data-tab]'); if (tab) { fire('Tab:' + tab.dataset.tab); return; }
    const tgt = e.target.closest('[data-tgt]'); if (tgt) { fire('Tgt:' + tgt.dataset.tgt); return; }
    const act = e.target.closest('[data-act]'); if (act) { fire(act.dataset.act); return; }
    const row = e.target.closest('[data-row]'); if (row) { const sc = el.style.getPropertyValue('--screen').trim() || 'bridge'; fire('Hover:' + sc + ':' + row.dataset.row); fire('Enter'); return; }
  }
  function onOver(e) { const row = e.target.closest('[data-row]'); if (row) { const sc = el.style.getPropertyValue('--screen').trim() || 'bridge'; fire('Hover:' + sc + ':' + row.dataset.row); } }
  function onCtx(e) { e.preventDefault(); fire('Escape'); }
  el.addEventListener('click', onClick); el.addEventListener('mouseover', onOver); el.addEventListener('contextmenu', onCtx);
  el.addEventListener('pointerdown', (e) => e.stopPropagation(), true);

  return { el, show, paint, combat() {}, debrief, setTheme() {}, dispose() { try { el.remove(); canvas.style.zIndex = ''; canvas.style.position = ''; } catch {} } };
}

const SCR = ['bridge', 'map', 'market', 'shipyard', 'missions'];
function hints(sc) {
  const k = (keys, label) => `<span>${keys.map((x) => `<kbd>${esc(x)}</kbd>`).join('/')} ${esc(tr(label))}</span>`;
  if (sc === 'map') return k(['←→'], 'cz_h_target') + k(['↑↓'], 'cz_h_action') + k(['Enter', 'A'], 'cz_h_jump') + k(['Esc', 'B'], 'cz_h_back');
  if (sc === 'market') return k(['↑↓'], 'cz_h_good') + k(['←→', 'X'], 'cz_h_buysell') + k([',', '.'], 'cz_h_qty') + k(['R', 'Y'], 'cz_h_fuel') + k(['Enter', 'A'], 'cz_h_ok');
  if (sc === 'shipyard') return k(['↑↓'], 'cz_h_pick') + k(['Enter', 'A'], 'cz_h_buy') + k(['Esc'], 'cz_h_back');
  if (sc === 'missions') return k(['↑↓'], 'cz_h_pick') + k(['Enter', 'A'], 'cz_h_launch') + k(['Esc', 'B'], 'cz_h_bridge');
  return k(['↑↓'], 'cz_h_pick') + k(['Enter', 'A'], 'cz_h_open') + k(['1-5'], 'cz_h_sections');
}

// ---- screen builders (return HTML for [data-body]) ---------------------------------------------
function repBars(m, r) {
  return `<div class="cz-rep">` + m.CONTENT.factions.map((f, i) => { const v = r.rep[i]; const w = Math.abs(v) / 2; const col = v >= 0 ? '#76e68c' : '#ff5c50'; return `<span style="color:${FAC[i]}">${esc(facName(i))}</span><span class="b"><i style="${v >= 0 ? 'left:50%' : 'right:50%'};width:${w}%;background:${col}"></i></span><span style="color:#9fb0c4">${v}</span>`; }).join('') + `</div>`;
}
const dots = (n) => { let s = ''; for (let i = 0; i < 4; i++) s += `<s class="${i < n ? 'on' : ''}"></s>`; return `<span class="cz-pips">${s}</span>`; };
function bBridge(m, st, sys) {
  const r = m.run;
  const acts = [['cz_b_map', 'cz_b_map_sub'], ['cz_b_market', 'cz_b_market_sub'], ['cz_b_shipyard', 'cz_b_shipyard_sub'], ['cz_b_missions', 'cz_b_missions_sub'], ['cz_b_launch', 'cz_b_launch_sub']];
  const list = acts.map((a, i) => `<div class="cz-row ${st.focus.bridge === i ? 'sel' : ''}" data-row="${i}"><b>${esc(tr(a[0], { n: m.missions ? m.missions.length : 0 }))}</b><span class="cz-sub" style="margin:0 0 0 auto">${esc(tr(a[1]))}</span></div>`).join('');
  const f = sys ? sys.faction : 0;
  return `<div class="cz-col" style="flex:1.1"><div class="cz-panel cz-card">
    <div class="cz-hero" style="background-image:linear-gradient(135deg,${FAC[f]}55,#0b1424 70%)"><div class="em" style="background-color:${FAC[f]}33"></div><b>${sys ? esc(sys.it) : ''}</b></div>
    <div style="display:flex;gap:8px;margin-bottom:10px"><span class="cz-chip" style="background:${FAC[f]}33;color:${FAC[f]}">${esc(facName(f))}</span><span class="cz-chip" style="background:${ECO[sys ? sys.econ : 0]}22;color:${ECO[sys ? sys.econ : 0]}">${sys ? esc(econName(sys.econ)) : ''}</span>${sys && sys.beacon ? `<span class="cz-chip" style="color:#ffbe40">${esc(tr('cz_ui_beacon'))}</span>` : ''}</div>
    <div style="display:grid;grid-template-columns:1fr 1fr;gap:8px 18px;font-size:13px;color:#cfd8e6">
      <span>${esc(tr('cz_ui_credits'))} <b style="color:#ffbe40">${r.credits}</b></span><span>${esc(tr('cz_ui_hull'))} <b>${r.hull}/${r.hull_max}</b></span>
      <span>${esc(tr('cz_ui_shield'))} <b style="color:#60cee8">${r.shield_max}</b></span><span>${esc(tr('cz_ui_laser'))} ${dots(r.weapon)}</span>
      <span>${esc(tr('cz_ui_jump'))} <b>${r.jump_range}</b></span><span>${esc(tr('cz_ui_sensors'))} ${dots(r.sensors)}</span></div>
    <h3 style="margin-top:14px">${esc(tr('cz_ui_reputation'))}</h3>${repBars(m, r)}</div></div>
   <div class="cz-col" style="flex:1"><div class="cz-panel cz-card"><h3>${esc(tr('cz_ui_command'))}</h3><div class="cz-list">${list}</div></div></div>`;
}
function bMap(m, st, sys) {
  const r = m.run, S = m.sector;
  let nodes = '', links = '';
  for (let i = 0; i < S.length; i++) {   // lit routes: links between lit beacon systems in range
    for (let j = i + 1; j < S.length; j++) {
      const a = S[i], b = S[j], d = Math.hypot(a.x - b.x, a.y - b.y);
      if (d > 30) continue;
      const ang = Math.atan2(b.y - a.y, b.x - a.x) * 180 / Math.PI;
      links += `<div class="cz-link" style="left:${a.x}%;top:${a.y}%;width:${d}%;transform:rotate(${ang}deg)"></div>`;
    }
  }
  for (let i = 0; i < S.length; i++) {
    const s = S[i], cur = i === r.sys, tg = i === st.target;
    const lit = (r.beacon_lit >>> 0) & (1 << i);
    const inR = m.econ.sysDist(m.CONTENT, r.sys, i) <= r.jump_range;
    const bk = s.beacon ? `<span class="bk" style="border-color:${lit ? '#ffbe40' : '#ff5c50'};border-style:${lit ? 'solid' : 'dashed'}"></span>` : '';
    nodes += `<div class="cz-node ${cur ? 'cur' : ''} ${tg ? 'tgt' : ''}" data-tgt="${i}" style="left:${s.x}%;top:${s.y}%;background:${FAC[s.faction]};opacity:${inR || cur ? 1 : .45}">${bk}${(cur || tg) ? `<span class="lbl">${esc(s.it)}</span>` : ''}</div>`;
  }
  const t = st.target, ts = S[t] || sys, d = m.econ.sysDist(m.CONTENT, r.sys, t), cost = m.econ.jumpCost(d), inR = d <= r.jump_range, aff = r.fuel >= cost;
  const acts = ['jump']; const showRel = sys && sys.beacon && !((r.beacon_lit >>> 0) & (1 << r.sys)); if (showRel) acts.push('relight'); acts.push('back');
  const labels = { jump: [tr('cz_m_jump'), inR && aff ? tr('cz_m_jump_cost', { c: cost, d: Math.round(d) }) : (!inR ? tr('cz_m_out_of_range') : tr('cz_m_no_cells'))], relight: [tr('cz_m_relight'), tr('cz_m_relight_cost')], back: [tr('cz_tab_bridge'), ''] };
  const list = acts.map((a, i) => `<div class="cz-row ${st.focus.map === i ? 'sel' : ''} ${a === 'jump' && (!inR || !aff) ? 'dis' : ''}" data-row="${i}"><b>${esc(labels[a][0])}</b><span class="cz-sub" style="margin:2px 0 0 auto">${esc(labels[a][1])}</span></div>`).join('');
  return `<div class="cz-panel cz-map" style="padding:8px">${links}${nodes}</div>
   <div class="cz-col" style="flex:0 0 260px"><div class="cz-panel cz-card"><h3>${ts ? esc(ts.it) : ''}</h3>
     <div style="display:flex;gap:6px;margin-bottom:8px;flex-wrap:wrap"><span class="cz-chip" style="color:${FAC[ts ? ts.faction : 0]}">${ts ? esc(facName(ts.faction)) : ''}</span><span class="cz-chip" style="color:${ECO[ts ? ts.econ : 0]}">${ts ? esc(econName(ts.econ)) : ''}</span>${ts && ts.beacon ? `<span class="cz-chip" style="color:#ffbe40">${esc(tr('cz_ui_beacon'))}</span>` : ''}</div>
     <div class="cz-list">${list}</div></div></div>`;
}
function bMarket(m, st, sys) {
  const r = m.run, used = m.econ.cargoUsed(r);
  let rows = '';
  for (let g = 0; g < m.CONTENT.goods.length; g++) {
    const buy = m.econ.unitBuy(m.CONTENT, r.sys, g, r.epoch, r.rep), sell = m.econ.unitSell(m.CONTENT, r.sys, g, r.epoch, r.rep);
    const canBuy = r.credits >= buy && used < r.cargo_max, sel = st.focus.market === g;
    rows += `<div class="cz-row ${sel ? 'sel' : ''}" data-row="${g}"><b>${esc(goodName(g))}</b>
      <span style="margin-left:auto;display:flex;gap:10px;align-items:center;font-weight:700">
        <span class="cz-cell ${sel && !st.marketCol ? 'on' : ''}" data-act="Buy:${g}" style="color:${canBuy ? '#ffbe40' : '#7c899b'}">${esc(tr('cz_k_buy_s'))} ${buy}</span>
        <span class="cz-cell ${sel && st.marketCol ? 'on' : ''}" data-act="Sell:${g}" style="color:${r.cargo[g] > 0 ? '#76e68c' : '#7c899b'}">${esc(tr('cz_k_sell_s'))} ${sell}</span>
        <span style="color:${r.cargo[g] ? '#ffbe40' : '#7c899b'};min-width:34px;text-align:right">×${r.cargo[g]}</span></span></div>`;
  }
  const fg = m.CONTENT.goods.length, price = m.econ.refuelPrice(m.CONTENT, r.sys);
  rows += `<div class="cz-row ${st.focus.market === fg ? 'sel' : ''}" data-row="${fg}"><b>${esc(tr('cz_ui_fuel'))}</b><span style="margin-left:auto;font-weight:700;color:#60cee8">${esc(tr('cz_ui_per_cell', { p: price }))}</span><span style="margin-left:14px;color:#ffbe40">${r.fuel}/${r.fuel_max}</span></div>`;
  return `<div class="cz-panel cz-card" style="flex:1"><h3>${esc(tr('cz_tab_market'))} · ${sys ? esc(sys.it) : ''} <span style="float:right;color:#9fb0c4;font-weight:700">${esc(tr(st.marketCol ? 'cz_ui_selling' : 'cz_ui_buying'))} · ${esc(tr('cz_ui_qty', { n: st.marketQty }))} · ${esc(tr('cz_ui_hold', { a: used, b: r.cargo_max }))}</span></h3><div class="cz-list">${rows}</div></div>`;
}
function bShipyard(m, st) {
  const r = m.run; let rows = '';
  for (let i = 0; i < m.shop.SHOP.length; i++) {
    const it = m.shop.SHOP[i], maxed = m.shop.shopMaxed(it.key, r), cost = m.shop.shopCost(it.key, r), lv = Math.max(0, Math.floor(it.level(r))), aff = !maxed && r.credits >= cost;
    rows += `<div class="cz-row ${st.focus.shipyard === i ? 'sel' : ''} ${maxed ? 'dis' : ''}" data-row="${i}"><b>${esc(tr(SHOPK[it.key]) || it.label[0])}</b><span style="margin:0 8px 0 14px">${dots(lv)}</span><span class="r" style="color:${maxed ? '#7c899b' : (aff ? '#ffbe40' : '#ff5c50')}">${maxed ? 'MAX' : cost + ' cr'}</span></div>`;
  }
  const ri = m.shop.SHOP.length, rc = m.shop.repairCost(r), intact = r.hull >= r.hull_max;
  rows += `<div class="cz-row ${st.focus.shipyard === ri ? 'sel' : ''} ${intact ? 'dis' : ''}" data-row="${ri}"><b>${esc(tr('cz_ui_repair'))}</b><span class="cz-sub" style="margin:2px 0 0 14px">${r.hull}/${r.hull_max}</span><span class="r" style="color:${intact ? '#7c899b' : (r.credits >= rc ? '#ffbe40' : '#ff5c50')}">${intact ? esc(tr('cz_ui_intact')) : rc + ' cr'}</span></div>`;
  return `<div class="cz-panel cz-card" style="flex:1"><h3>${esc(tr('cz_tab_shipyard'))} · ${r.credits} cr</h3><div class="cz-list">${rows}</div></div>`;
}
function bMissions(m, st, sys) {
  const ms = m.missions || []; let rows = '';
  for (let i = 0; i < ms.length; i++) {
    const mi = ms[i], fl = mi.flavor || {}, ft = flavorText(mi), rc = RARC[fl.rarity | 0];
    const mods = (ft.mods || []).map((x) => `<span class="cz-chip" style="font-size:10px;background:rgba(255,190,64,.12);color:#ffbe40">${esc(x)}</span>`).join(' ');
    const giver = tr('cz_spk_control_' + mi.offer_fac);
    rows += `<div class="cz-row ${st.focus.missions === i ? 'sel' : ''}" data-row="${i}"><div style="min-width:0;flex:1">`
      + `<b style="color:${rc}">${fl.star ? esc(fl.star) + ' ' : ''}${esc(ft.title)}${sys ? ' · ' + esc(sys.it) : ''}</b>`
      + `<div class="cz-sub" style="color:#aeb9c8">${esc(ft.brief)}</div>`
      + `<div class="cz-sub" style="margin-top:3px">${esc(tr('cz_ui_hostiles', { w: mi.waves, p: mi.per_wave }))}${mi.ace ? ' · ' + esc(tr('cz_hud_ace')) : ''} · vs <span style="color:${FAC[mi.foe_fac]}">${esc(facName(mi.foe_fac))}</span> ${mods}</div>`
      + `<div class="cz-sub" style="margin-top:2px;color:#8fa3b8">${esc(tr('cz_ui_giver', { name: giver }))}</div>`
      + `</div><span class="r" style="margin-left:auto;text-align:right"><span style="font-size:10px;display:block;color:${rc};opacity:.85">${esc(ft.rarity || '')}</span><span style="color:#ffbe40">${mi.reward_cr} cr</span></span></div>`;
  }
  rows += `<div class="cz-row ${st.focus.missions === ms.length ? 'sel' : ''}" data-row="${ms.length}"><div><b>${esc(tr('cz_ui_free_patrol'))}</b><div class="cz-sub">${esc(tr('cz_ui_free_patrol_sub'))}</div></div><span class="r" style="color:#9fb0c4;margin-left:auto">${esc(tr('cz_ui_salvage'))}</span></div>`;
  return `<div class="cz-panel cz-card" style="flex:1"><h3>${esc(tr('cz_ui_mission_bay'))} · ${sys ? esc(sys.it) : ''}</h3><div class="cz-list">${rows}</div></div>`;
}
