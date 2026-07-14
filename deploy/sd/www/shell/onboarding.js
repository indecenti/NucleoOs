// NucleoOS — first-boot onboarding wizard.
//
// Shows ONCE (per browser) on the first paired boot when no AI provider is configured: a curated
// welcome, a NucleoOS install tutorial (PWA / how the OS is served), and a guided Claude/Groq API-key
// setup with a live key test — all browser-direct (the Cardputer is never loaded). Resolves when the
// user finishes or skips. Idempotent: a `nucleo.onboarded` flag + the device /api/anima/caps probe
// keep it from ever nagging again once a key exists.

import * as AI from './ai.js';

const ONB_KEY = 'nucleo.onboarded';
const lang = () => (localStorage.getItem('anima.lang') === 'en' ? 'en' : 'it');
const EN = () => lang() === 'en';

// tiny DOM builder (XSS-safe: text via textContent, no innerHTML of user data)
function h(tag, props, ...kids) {
  const e = document.createElement(tag);
  if (props) for (const k in props) {
    if (k === 'class') e.className = props[k];
    else if (k === 'html') e.innerHTML = props[k];           // only for trusted static strings
    else if (k === 'on') for (const ev in props.on) e.addEventListener(ev, props.on[ev]);
    else if (k in e) e[k] = props[k]; else e.setAttribute(k, props[k]);
  }
  for (const kid of kids) if (kid != null) e.append(kid.nodeType ? kid : document.createTextNode(kid));
  return e;
}

export async function ensureOnboarding() {
  try {
    if (localStorage.getItem(ONB_KEY) === '1') return;
    const c = await AI.caps();
    if (c && c.hasKey) { localStorage.setItem(ONB_KEY, '1'); return; }   // already set up (e.g. another browser)
  } catch {}
  return runWizard();
}

function runWizard() {
  return new Promise((resolve) => {
    let en = EN();   // recomputed each render() — the first step lets the user pick the OS language
    const state = { step: 0, provider: 'anthropic', model: AI.PROVIDERS.anthropic.def, key: '', exec: 'browser' };
    const STEPS = ['language', 'welcome', 'install', 'provider', 'key'];

    const card = h('div', { id: 'nob-card' });
    const scrim = h('div', { id: 'nob-scrim' }, card);
    const done = () => { localStorage.setItem(ONB_KEY, '1'); scrim.remove(); resolve(); };

    function header(title, sub) {
      return h('div', { class: 'nob-hd' },
        h('div', { class: 'nob-logo' }, '◆'),
        h('div', null, h('h1', null, title), h('p', { class: 'nob-sub' }, sub)));
    }
    function dots() {
      const total = state.provider === 'offline' ? STEPS.length - 1 : STEPS.length;
      const wrap = h('div', { class: 'nob-dots' });
      for (let i = 0; i < total; i++) wrap.append(h('i', { class: i === state.step ? 'on' : '' }));
      return wrap;
    }

    function render() {
      en = EN();                                   // reflect a language change made on the first step
      card.textContent = '';
      if (STEPS[state.step] === 'language') renderLanguage();
      else if (STEPS[state.step] === 'welcome') renderWelcome();
      else if (STEPS[state.step] === 'install') renderInstall();
      else if (STEPS[state.step] === 'provider') renderProvider();
      else renderKey();
    }

    // ---- step 0: language ----------------------------------------------------
    // A modern OS asks for the language first. The choice flows through the centralized i18n engine
    // (window.NucleoI18N) so it persists OS-wide; the rest of the wizard then renders in that language.
    function renderLanguage() {
      const langs = (window.NucleoI18N && window.NucleoI18N.LANGS)
        || [{ code: 'it', label: 'Italiano', flag: '🇮🇹' }, { code: 'en', label: 'English', flag: '🇬🇧' }];
      const cur = localStorage.getItem('anima.lang') === 'en' ? 'en' : 'it';
      const pick = (code) => {
        if (window.NucleoI18N) window.NucleoI18N.setLang(code);
        else { try { localStorage.setItem('anima.lang', code); } catch {} }
        render();
      };
      card.append(
        header(en ? 'Choose your language' : 'Scegli la lingua',
               en ? 'You can change it anytime in Settings ▸ Appearance' : 'Puoi cambiarla quando vuoi in Impostazioni ▸ Aspetto'),
        h('div', { class: 'nob-body' }, h('div', { class: 'nob-choices' },
          ...langs.map((l) => h('button', { class: 'nob-choice' + (cur === l.code ? ' sel' : ''),
            on: { click: () => pick(l.code) } },
            h('div', { class: 'cic' }, l.flag),
            h('div', null, h('div', { class: 'ct' }, l.label)))))),
        footer({ next: true }));
    }

    // ---- step 1: welcome -----------------------------------------------------
    function renderWelcome() {
      card.append(
        header(en ? 'Welcome to NucleoOS' : 'Benvenuto in NucleoOS',
               en ? 'A web-native OS that lives on your Cardputer' : 'Un OS web-native che vive sul tuo Cardputer'),
        h('div', { class: 'nob-body' },
          h('p', { html: en
            ? 'NucleoOS turns your M5Stack Cardputer into a tiny computer you drive from this browser. At its heart is <b>ANIMA</b> — an assistant whose brain runs <b>offline, on the device itself</b>.'
            : 'NucleoOS trasforma il tuo M5Stack Cardputer in un piccolo computer che guidi da questo browser. Al centro c’è <b>ANIMA</b> — un assistente il cui cervello gira <b>offline, sul dispositivo stesso</b>.' }),
          h('div', { class: 'nob-feat' },
            feat('🧠', en ? 'Offline brain' : 'Cervello offline', en ? 'Answers, reasoning and memory — no cloud needed.' : 'Risposte, ragionamento e memoria — senza cloud.'),
            feat('☁', en ? 'Optional cloud' : 'Cloud opzionale', en ? 'Add a Claude key for deeper understanding & verification.' : 'Aggiungi una chiave Claude per comprensione e verifica migliori.'),
            feat('🔒', en ? 'Your keys, your device' : 'Le tue chiavi, il tuo device', en ? 'Keys stay on the SD; cloud calls go straight from your browser.' : 'Le chiavi restano sulla SD; le chiamate cloud partono dal tuo browser.'),
            feat('⚡', en ? 'Light on the Cardputer' : 'Leggero sul Cardputer', en ? 'The heavy AI work runs in your browser, not the chip.' : 'Il lavoro pesante dell’IA gira nel browser, non sul chip.')),
          h('p', { class: 'nob-note', html: en
            ? '✨ Two minutes now and ANIMA is ready everywhere in the OS.' : '✨ Due minuti adesso e ANIMA è pronta ovunque nell’OS.' })),
        footer({ next: true, skip: true }));
    }
    function feat(ic, t, d) {
      return h('div', null, h('div', { class: 'ic' }, ic), h('div', { class: 't' }, t), h('div', { class: 'd' }, d));
    }

    // ---- step 1: install NucleoOS -------------------------------------------
    function renderInstall() {
      const prompt = window.__nucleoInstallPrompt;
      const body = h('div', { class: 'nob-body' },
        h('p', { html: en
          ? 'NucleoOS is served by the Cardputer over your network — no app store. <b>Install it</b> to get a full-screen app icon and instant launch.'
          : 'NucleoOS è servito dal Cardputer sulla tua rete — niente app store. <b>Installalo</b> per avere un’icona a tutto schermo e avvio immediato.' }),
        h('ol', { class: 'nob-steps' },
          li(en ? 'Keep this Cardputer on the same Wi-Fi as this device.' : 'Tieni il Cardputer sulla stessa Wi-Fi di questo dispositivo.'),
          li(en ? 'Desktop (Chrome/Edge): click the ⊕ / install icon in the address bar.' : 'Desktop (Chrome/Edge): clicca l’icona ⊕ / installa nella barra indirizzi.'),
          li(en ? 'iPhone/iPad (Safari): Share → “Add to Home Screen”.' : 'iPhone/iPad (Safari): Condividi → “Aggiungi a Home”.'),
          li(en ? 'Android (Chrome): menu ⋮ → “Install app”.' : 'Android (Chrome): menu ⋮ → “Installa app”.')),
        h('p', { class: 'nob-note', html: en
          ? '💡 Tip: bookmark the device IP so you can always get back to NucleoOS.' : '💡 Suggerimento: salva l’IP del dispositivo per tornare sempre a NucleoOS.' }));
      card.append(header(en ? 'Install NucleoOS' : 'Installa NucleoOS', en ? 'Add it like a native app' : 'Aggiungilo come un’app nativa'), body);
      const extra = [];
      if (prompt) extra.push(h('button', { class: 'nob-btn', on: { click: async () => { try { prompt.prompt(); await prompt.userChoice; } catch {} window.__nucleoInstallPrompt = null; } } }, en ? '⤓ Install app' : '⤓ Installa app'));
      card.append(footer({ back: true, next: true, skip: true, extra }));
    }
    function li(...kids) { return h('li', null, ...kids); }

    // ---- step 2: choose provider --------------------------------------------
    function renderProvider() {
      const mk = (id, ic, title, desc, reco) => h('button', { class: 'nob-choice' + (state.provider === id ? ' sel' : ''), 'data-id': id,
        on: { click: () => { state.provider = id; render(); } } },
        h('div', { class: 'cic' }, ic),
        h('div', null, h('div', { class: 'ct', html: title + (reco ? '<span class="reco">' + (en ? 'RECOMMENDED' : 'CONSIGLIATO') + '</span>' : '') }), h('div', { class: 'cd' }, desc)));
      card.append(
        header(en ? 'Choose your AI' : 'Scegli la tua IA', en ? 'You can change this anytime in Settings ▸ AI' : 'Puoi cambiarlo quando vuoi in Impostazioni ▸ IA'),
        h('div', { class: 'nob-body' }, h('div', { class: 'nob-choices' },
          mk('anthropic', '🟣', 'Claude (Anthropic)', en ? 'Best understanding. Runs in your browser — zero load on the Cardputer.' : 'Comprensione al top. Gira nel browser — zero carico sul Cardputer.', true),
          mk('openai', '⚡', 'Groq', en ? 'Fast & free tier. OpenAI-compatible (Llama models).' : 'Veloce e con piano gratuito. OpenAI-compatibile (modelli Llama).', false),
          mk('google', '✨', 'Gemini (Google)', en ? 'Strong, generous free tier. Routed via the device proxy (no browser CORS).' : 'Forte, free tier generoso. Instradato dal proxy del device (niente CORS browser).', false),
          mk('offline', '🧠', en ? 'Offline only' : 'Solo offline', en ? 'No key. ANIMA still answers from its on-device brain, weather & Wikipedia.' : 'Nessuna chiave. ANIMA risponde comunque dal cervello on-device, meteo e Wikipedia.', false))),
        footer({ back: true, next: state.provider !== 'offline', finish: state.provider === 'offline' }));
    }

    // ---- step 3: API key -----------------------------------------------------
    function renderKey() {
      const p = AI.providerOf(state.provider);
      if (!state.model || !p.models.some((m) => m[0] === state.model)) state.model = p.def;
      const stat = h('div', { class: 'nob-stat' });
      const input = h('input', { class: 'nob-input', type: 'password', placeholder: p.ph, autocomplete: 'off', spellcheck: 'false' });
      const sel = h('select', { class: 'nob-select' });
      for (const m of p.models) sel.append(h('option', { value: m[0], selected: m[0] === state.model }, AI.modelLabel(m)));
      sel.addEventListener('change', () => { state.model = sel.value; });

      const cfg = () => ({ provider: state.provider, base: p.base, model: sel.value, key: input.value.trim(), version: p.version, exec: 'browser' });
      const setStat = (t, cls) => { stat.textContent = t; stat.className = 'nob-stat' + (cls ? ' ' + cls : ''); };

      const testBtn = h('button', { class: 'nob-btn', on: { click: async () => {
        const k = input.value.trim(); if (!k) { setStat(en ? 'Paste a key first.' : 'Incolla prima una chiave.', 'err'); return; }
        setStat(en ? 'testing…' : 'provo…'); testBtn.disabled = true;
        try { const ok = await AI.cloudPing(cfg()); setStat(ok ? (en ? '✓ Key works!' : '✓ La chiave funziona!') : (en ? '✗ Key rejected.' : '✗ Chiave rifiutata.'), ok ? 'ok' : 'err'); }
        catch (e) { setStat('✗ ' + String(e.message || e), 'err'); }
        finally { testBtn.disabled = false; }
      } } }, en ? '⚡ Test' : '⚡ Prova');

      const saveBtn = h('button', { class: 'nob-btn pri', on: { click: async () => {
        const k = input.value.trim();
        if (!k) { setStat(en ? 'Paste your key.' : 'Incolla la tua chiave.', 'err'); return; }
        if (!p.prefix.test(k) && !confirm(en ? 'That doesn’t look like a ' + p.label + ' key. Save anyway?' : 'Non sembra una chiave ' + p.label + '. Salvo lo stesso?')) return;
        setStat(en ? 'saving…' : 'salvo…'); saveBtn.disabled = true;
        const res = await AI.writeTeacher(cfg());
        saveBtn.disabled = false;
        if (res === true && state.provider === 'google') {
          // Detect the Gemini plan (real models + paid/free tier) and re-save with the recommended model + tier.
          // Best-effort: if it can't reach the API it just keeps the Flash default and finishes anyway.
          setStat(en ? 'detecting plan…' : 'rilevo il piano…');
          try {
            const c = await AI.calibrateGemini({ base: p.base, key: k });
            const d = cfg(); if (!c.models.includes(d.model)) d.model = c.recommended; d.geminiTier = c.tier;
            await AI.writeTeacher(d);
            setStat('✓ ' + AI.geminiTierLabel(c.tier, en), 'ok');
          } catch { setStat(en ? '✓ Saved — ANIMA is ready.' : '✓ Salvata — ANIMA è pronta.', 'ok'); }
          setTimeout(done, 1000); return;
        }
        if (res === true) { setStat(en ? '✓ Saved — ANIMA is ready.' : '✓ Salvata — ANIMA è pronta.', 'ok'); setTimeout(done, 650); }
        else if (res === 'unpaired') setStat(en ? 'Pair this browser first.' : 'Accoppia prima il browser.', 'err');
        else setStat(en ? 'Save failed.' : 'Salvataggio fallito.', 'err');
      } } }, en ? '💾 Save & finish' : '💾 Salva e finisci');

      const where = state.provider === 'anthropic'
        ? [li(en ? 'Go to ' : 'Vai su ', h('a', { href: 'https://console.anthropic.com/settings/keys', target: '_blank', rel: 'noopener' }, 'console.anthropic.com')),
           li(en ? 'Open API Keys → Create Key.' : 'Apri API Keys → Create Key.'),
           li(en ? 'Copy the key (starts with sk-ant-) and paste it below.' : 'Copia la chiave (inizia con sk-ant-) e incollala qui sotto.')]
        : state.provider === 'google'
        ? [li(en ? 'Go to ' : 'Vai su ', h('a', { href: 'https://aistudio.google.com/apikey', target: '_blank', rel: 'noopener' }, 'aistudio.google.com/apikey')),
           li(en ? 'Create an API key (starts with AIza) and paste it below.' : 'Crea una API key (inizia con AIza) e incollala qui sotto.'),
           li(en ? 'The Cardputer relays the call (Google has no browser access).' : 'Il Cardputer inoltra la chiamata (Google non è accessibile dal browser).')]
        : [li(en ? 'Go to ' : 'Vai su ', h('a', { href: 'https://console.groq.com/keys', target: '_blank', rel: 'noopener' }, 'console.groq.com')),
           li(en ? 'Create an API key (starts with gsk_) and paste it below.' : 'Crea una API key (inizia con gsk_) e incollala qui sotto.')];

      input.addEventListener('keydown', (e) => { if (e.key === 'Enter') { e.preventDefault(); saveBtn.click(); } });

      card.append(
        header((en ? 'Connect ' : 'Collega ') + p.label, en ? 'Your key stays on the device' : 'La chiave resta sul dispositivo'),
        h('div', { class: 'nob-body' },
          h('ol', { class: 'nob-steps' }, ...where),
          h('div', { class: 'nob-field' },
            h('label', null, en ? 'Model' : 'Modello'), sel,
            h('label', null, 'API key'), input,
            h('div', { class: 'nob-row' }, testBtn), stat),
          h('div', { class: 'nob-note', html: '🔒 ' + (en
            ? 'Stored only on your device (SD), never logged. With Claude the key stays in <b>your browser</b> and talks directly to Anthropic — the Cardputer isn’t loaded.'
            : 'Salvata solo sul tuo dispositivo (SD), mai nei log. Con Claude la chiave resta nel <b>tuo browser</b> e parla direttamente con Anthropic — il Cardputer non viene caricato.') })),
        footer({ back: true, saveSlot: saveBtn }));
    }

    // ---- footer --------------------------------------------------------------
    function footer(opts) {
      const btns = h('div', { class: 'nob-btns' });
      if (opts.extra) for (const e of opts.extra) btns.append(e);
      if (opts.skip) btns.append(h('button', { class: 'nob-btn ghost', on: { click: () => { if (confirm(en ? 'Skip AI setup? You can do it later in Settings ▸ AI.' : 'Salto la configurazione IA? Puoi farla dopo in Impostazioni ▸ IA.')) done(); } } }, en ? 'Set up later' : 'Più tardi'));
      if (opts.back) btns.append(h('button', { class: 'nob-btn', on: { click: () => { state.step = Math.max(0, state.step - 1); render(); } } }, en ? '← Back' : '← Indietro'));
      if (opts.saveSlot) btns.append(opts.saveSlot);
      if (opts.next) btns.append(h('button', { class: 'nob-btn pri', on: { click: () => { state.step++; render(); } } }, en ? 'Next →' : 'Avanti →'));
      if (opts.finish) btns.append(h('button', { class: 'nob-btn pri', on: { click: done } }, en ? 'Start using NucleoOS' : 'Inizia a usare NucleoOS'));
      return h('div', { class: 'nob-ft' }, dots(), btns);
    }

    render();
    document.body.append(scrim);
  });
}
