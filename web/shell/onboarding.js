// NucleoOS — first-boot onboarding wizard.
//
// Shows ONCE (per browser) on the first paired boot when no AI provider is configured: a curated
// welcome, a NucleoOS install tutorial (PWA / how the OS is served), and a guided Claude/Groq API-key
// setup with a live key test — all browser-direct (the Cardputer is never loaded). Resolves when the
// user finishes or skips. Idempotent: a `nucleo.onboarded` flag + the device /api/anima/caps probe
// keep it from ever nagging again once a key exists.

import * as AI from './ai.js';

const ONB_KEY = 'nucleo.onboarded';
const CODES = ['it', 'en', 'es', 'fr', 'de'];
const lang = () => { const l = String(localStorage.getItem('anima.lang') || 'it').slice(0, 2); return CODES.includes(l) ? l : 'it'; };
// es/fr/de overlays, keyed by the ENGLISH UI string. it/en render inline via the TR ternaries below;
// TR(it, en): it→it, en→en, otherwise L10N[lang][en] (falling back to the English text if a key is missing).
const L10N = {
  es: {
    'Choose your language': 'Elige tu idioma',
    'You can change it anytime in Settings ▸ Appearance': 'Puedes cambiarlo cuando quieras en Ajustes ▸ Apariencia',
    'Welcome to NucleoOS': 'Bienvenido a NucleoOS',
    'A web-native OS that lives on your Cardputer': 'Un SO web-nativo que vive en tu Cardputer',
    'NucleoOS turns your M5Stack Cardputer into a tiny computer you drive from this browser. At its heart is <b>ANIMA</b> — an assistant whose brain runs <b>offline, on the device itself</b>.': 'NucleoOS convierte tu M5Stack Cardputer en un pequeño ordenador que controlas desde este navegador. En su núcleo está <b>ANIMA</b> — un asistente cuyo cerebro funciona <b>sin conexión, en el propio dispositivo</b>.',
    'Offline brain': 'Cerebro sin conexión',
    'Answers, reasoning and memory — no cloud needed.': 'Respuestas, razonamiento y memoria — sin necesidad de nube.',
    'Optional cloud': 'Nube opcional',
    'Add a Claude key for deeper understanding & verification.': 'Añade una clave Claude para mayor comprensión y verificación.',
    'Your keys, your device': 'Tus claves, tu dispositivo',
    'Keys stay on the SD; cloud calls go straight from your browser.': 'Las claves quedan en la SD; las llamadas a la nube salen directas desde tu navegador.',
    'Light on the Cardputer': 'Ligero para el Cardputer',
    'The heavy AI work runs in your browser, not the chip.': 'El trabajo pesado de la IA se ejecuta en tu navegador, no en el chip.',
    '✨ Two minutes now and ANIMA is ready everywhere in the OS.': '✨ Dos minutos ahora y ANIMA estará lista en todo el OS.',
    'NucleoOS is served by the Cardputer over your network — no app store. <b>Install it</b> to get a full-screen app icon and instant launch.': 'NucleoOS lo sirve el Cardputer a través de tu red — sin tienda de apps. <b>Instálalo</b> para tener un icono de app a pantalla completa e inicio instantáneo.',
    'Keep this Cardputer on the same Wi-Fi as this device.': 'Mantén este Cardputer en la misma Wi-Fi que este dispositivo.',
    'Desktop (Chrome/Edge): click the ⊕ / install icon in the address bar.': 'Escritorio (Chrome/Edge): haz clic en el icono ⊕ / instalar en la barra de direcciones.',
    'iPhone/iPad (Safari): Share → “Add to Home Screen”.': 'iPhone/iPad (Safari): Compartir → “Añadir a pantalla de inicio”.',
    'Android (Chrome): menu ⋮ → “Install app”.': 'Android (Chrome): menú ⋮ → “Instalar app”.',
    '💡 Tip: bookmark the device IP so you can always get back to NucleoOS.': '💡 Consejo: guarda la IP del dispositivo en marcadores para volver siempre a NucleoOS.',
    'Install NucleoOS': 'Instalar NucleoOS',
    'Add it like a native app': 'Añádelo como una app nativa',
    '⤓ Install app': '⤓ Instalar app',
    'RECOMMENDED': 'RECOMENDADO',
    'Choose your AI': 'Elige tu IA',
    'You can change this anytime in Settings ▸ AI': 'Puedes cambiarlo cuando quieras en Ajustes ▸ IA',
    'Best understanding. Runs in your browser — zero load on the Cardputer.': 'La mejor comprensión. Se ejecuta en tu navegador — cero carga en el Cardputer.',
    'Fast & free tier. OpenAI-compatible (Llama models).': 'Rápido y con nivel gratuito. Compatible con OpenAI (modelos Llama).',
    'Strong, generous free tier. Routed via the device proxy (no browser CORS).': 'Potente, con nivel gratuito generoso. Enrutado por el proxy del dispositivo (sin CORS de navegador).',
    'Offline only': 'Solo sin conexión',
    'No key. ANIMA still answers from its on-device brain, weather & Wikipedia.': 'Sin clave. ANIMA sigue respondiendo desde su cerebro en el dispositivo, el tiempo y Wikipedia.',
    'Paste a key first.': 'Pega primero una clave.',
    'testing…': 'probando…',
    '✓ Key works!': '✓ ¡La clave funciona!',
    '✗ Key rejected.': '✗ Clave rechazada.',
    '⚡ Test': '⚡ Probar',
    'Paste your key.': 'Pega tu clave.',
    'That doesn’t look like a ': 'No parece una clave ',
    ' key. Save anyway?': '. ¿Guardar de todos modos?',
    'saving…': 'guardando…',
    'detecting plan…': 'detectando plan…',
    '✓ Saved — ANIMA is ready.': '✓ Guardada — ANIMA está lista.',
    'Pair this browser first.': 'Empareja primero este navegador.',
    'Save failed.': 'Error al guardar.',
    '💾 Save & finish': '💾 Guardar y finalizar',
    'Go to ': 'Ve a ',
    'Open API Keys → Create Key.': 'Abre API Keys → Create Key.',
    'Copy the key (starts with sk-ant-) and paste it below.': 'Copia la clave (empieza por sk-ant-) y pégala abajo.',
    'Create an API key (starts with AIza) and paste it below.': 'Crea una clave API (empieza por AIza) y pégala abajo.',
    'The Cardputer relays the call (Google has no browser access).': 'El Cardputer retransmite la llamada (Google no tiene acceso desde el navegador).',
    'Create an API key (starts with gsk_) and paste it below.': 'Crea una clave API (empieza por gsk_) y pégala abajo.',
    'Connect ': 'Conecta ',
    'Your key stays on the device': 'Tu clave permanece en el dispositivo',
    'Model': 'Modelo',
    'Stored only on your device (SD), never logged. With Claude the key stays in <b>your browser</b> and talks directly to Anthropic — the Cardputer isn’t loaded.': 'Se guarda solo en tu dispositivo (SD), nunca en registros. Con Claude la clave permanece en <b>tu navegador</b> y habla directamente con Anthropic — el Cardputer no se carga.',
    'Skip AI setup? You can do it later in Settings ▸ AI.': '¿Omitir la configuración de la IA? Puedes hacerlo luego en Ajustes ▸ IA.',
    'Set up later': 'Más tarde',
    '← Back': '← Atrás',
    'Next →': 'Siguiente →',
    'Start using NucleoOS': 'Empezar a usar NucleoOS',
  },
  fr: {
    'Choose your language': 'Choisissez votre langue',
    'You can change it anytime in Settings ▸ Appearance': 'Vous pouvez la changer à tout moment dans Réglages ▸ Apparence',
    'Welcome to NucleoOS': 'Bienvenue dans NucleoOS',
    'A web-native OS that lives on your Cardputer': 'Un OS web-natif qui vit sur votre Cardputer',
    'NucleoOS turns your M5Stack Cardputer into a tiny computer you drive from this browser. At its heart is <b>ANIMA</b> — an assistant whose brain runs <b>offline, on the device itself</b>.': 'NucleoOS transforme votre M5Stack Cardputer en un petit ordinateur que vous pilotez depuis ce navigateur. En son cœur se trouve <b>ANIMA</b> — un assistant dont le cerveau fonctionne <b>hors ligne, sur l’appareil lui-même</b>.',
    'Offline brain': 'Cerveau hors ligne',
    'Answers, reasoning and memory — no cloud needed.': 'Réponses, raisonnement et mémoire — sans cloud.',
    'Optional cloud': 'Cloud optionnel',
    'Add a Claude key for deeper understanding & verification.': 'Ajoutez une clé Claude pour une compréhension et une vérification accrues.',
    'Your keys, your device': 'Vos clés, votre appareil',
    'Keys stay on the SD; cloud calls go straight from your browser.': 'Les clés restent sur la SD ; les appels cloud partent directement de votre navigateur.',
    'Light on the Cardputer': 'Léger pour le Cardputer',
    'The heavy AI work runs in your browser, not the chip.': 'Le gros du travail de l’IA s’exécute dans votre navigateur, pas sur la puce.',
    '✨ Two minutes now and ANIMA is ready everywhere in the OS.': '✨ Deux minutes maintenant et ANIMA est prête partout dans l’OS.',
    'NucleoOS is served by the Cardputer over your network — no app store. <b>Install it</b> to get a full-screen app icon and instant launch.': 'NucleoOS est servi par le Cardputer sur votre réseau — sans magasin d’applications. <b>Installez-le</b> pour obtenir une icône plein écran et un lancement instantané.',
    'Keep this Cardputer on the same Wi-Fi as this device.': 'Gardez ce Cardputer sur le même Wi-Fi que cet appareil.',
    'Desktop (Chrome/Edge): click the ⊕ / install icon in the address bar.': 'Ordinateur (Chrome/Edge) : cliquez sur l’icône ⊕ / installer dans la barre d’adresse.',
    'iPhone/iPad (Safari): Share → “Add to Home Screen”.': 'iPhone/iPad (Safari) : Partager → “Sur l’écran d’accueil”.',
    'Android (Chrome): menu ⋮ → “Install app”.': 'Android (Chrome) : menu ⋮ → “Installer l’application”.',
    '💡 Tip: bookmark the device IP so you can always get back to NucleoOS.': '💡 Astuce : ajoutez l’IP de l’appareil aux favoris pour toujours revenir à NucleoOS.',
    'Install NucleoOS': 'Installer NucleoOS',
    'Add it like a native app': 'Ajoutez-le comme une app native',
    '⤓ Install app': '⤓ Installer l’app',
    'RECOMMENDED': 'RECOMMANDÉ',
    'Choose your AI': 'Choisissez votre IA',
    'You can change this anytime in Settings ▸ AI': 'Vous pouvez le changer à tout moment dans Réglages ▸ IA',
    'Best understanding. Runs in your browser — zero load on the Cardputer.': 'La meilleure compréhension. S’exécute dans votre navigateur — aucune charge sur le Cardputer.',
    'Fast & free tier. OpenAI-compatible (Llama models).': 'Rapide et avec offre gratuite. Compatible OpenAI (modèles Llama).',
    'Strong, generous free tier. Routed via the device proxy (no browser CORS).': 'Puissant, offre gratuite généreuse. Acheminé via le proxy de l’appareil (sans CORS navigateur).',
    'Offline only': 'Hors ligne uniquement',
    'No key. ANIMA still answers from its on-device brain, weather & Wikipedia.': 'Sans clé. ANIMA répond quand même depuis son cerveau embarqué, la météo et Wikipédia.',
    'Paste a key first.': 'Collez d’abord une clé.',
    'testing…': 'test…',
    '✓ Key works!': '✓ La clé fonctionne !',
    '✗ Key rejected.': '✗ Clé refusée.',
    '⚡ Test': '⚡ Tester',
    'Paste your key.': 'Collez votre clé.',
    'That doesn’t look like a ': 'Cela ne ressemble pas à une clé ',
    ' key. Save anyway?': '. Enregistrer quand même ?',
    'saving…': 'enregistrement…',
    'detecting plan…': 'détection du forfait…',
    '✓ Saved — ANIMA is ready.': '✓ Enregistrée — ANIMA est prête.',
    'Pair this browser first.': 'Appairez d’abord ce navigateur.',
    'Save failed.': 'Échec de l’enregistrement.',
    '💾 Save & finish': '💾 Enregistrer et terminer',
    'Go to ': 'Allez sur ',
    'Open API Keys → Create Key.': 'Ouvrez API Keys → Create Key.',
    'Copy the key (starts with sk-ant-) and paste it below.': 'Copiez la clé (commence par sk-ant-) et collez-la ci-dessous.',
    'Create an API key (starts with AIza) and paste it below.': 'Créez une clé API (commence par AIza) et collez-la ci-dessous.',
    'The Cardputer relays the call (Google has no browser access).': 'Le Cardputer relaie l’appel (Google n’a pas d’accès navigateur).',
    'Create an API key (starts with gsk_) and paste it below.': 'Créez une clé API (commence par gsk_) et collez-la ci-dessous.',
    'Connect ': 'Connecter ',
    'Your key stays on the device': 'Votre clé reste sur l’appareil',
    'Model': 'Modèle',
    'Stored only on your device (SD), never logged. With Claude the key stays in <b>your browser</b> and talks directly to Anthropic — the Cardputer isn’t loaded.': 'Stockée uniquement sur votre appareil (SD), jamais journalisée. Avec Claude, la clé reste dans <b>votre navigateur</b> et communique directement avec Anthropic — le Cardputer n’est pas sollicité.',
    'Skip AI setup? You can do it later in Settings ▸ AI.': 'Passer la configuration de l’IA ? Vous pourrez le faire plus tard dans Réglages ▸ IA.',
    'Set up later': 'Plus tard',
    '← Back': '← Retour',
    'Next →': 'Suivant →',
    'Start using NucleoOS': 'Commencer à utiliser NucleoOS',
  },
  de: {
    'Choose your language': 'Wähle deine Sprache',
    'You can change it anytime in Settings ▸ Appearance': 'Du kannst sie jederzeit unter Einstellungen ▸ Erscheinungsbild ändern',
    'Welcome to NucleoOS': 'Willkommen bei NucleoOS',
    'A web-native OS that lives on your Cardputer': 'Ein web-natives OS, das auf deinem Cardputer lebt',
    'NucleoOS turns your M5Stack Cardputer into a tiny computer you drive from this browser. At its heart is <b>ANIMA</b> — an assistant whose brain runs <b>offline, on the device itself</b>.': 'NucleoOS verwandelt deinen M5Stack Cardputer in einen winzigen Computer, den du von diesem Browser aus steuerst. Im Kern steckt <b>ANIMA</b> — ein Assistent, dessen Gehirn <b>offline, auf dem Gerät selbst</b> läuft.',
    'Offline brain': 'Offline-Gehirn',
    'Answers, reasoning and memory — no cloud needed.': 'Antworten, Schlussfolgern und Gedächtnis — ohne Cloud.',
    'Optional cloud': 'Optionale Cloud',
    'Add a Claude key for deeper understanding & verification.': 'Füge einen Claude-Schlüssel für tieferes Verständnis und Verifizierung hinzu.',
    'Your keys, your device': 'Deine Schlüssel, dein Gerät',
    'Keys stay on the SD; cloud calls go straight from your browser.': 'Die Schlüssel bleiben auf der SD; Cloud-Aufrufe gehen direkt von deinem Browser aus.',
    'Light on the Cardputer': 'Schonend für den Cardputer',
    'The heavy AI work runs in your browser, not the chip.': 'Die schwere KI-Arbeit läuft in deinem Browser, nicht auf dem Chip.',
    '✨ Two minutes now and ANIMA is ready everywhere in the OS.': '✨ Zwei Minuten jetzt und ANIMA ist überall im OS bereit.',
    'NucleoOS is served by the Cardputer over your network — no app store. <b>Install it</b> to get a full-screen app icon and instant launch.': 'NucleoOS wird vom Cardputer über dein Netzwerk bereitgestellt — kein App-Store. <b>Installiere es</b>, um ein Vollbild-App-Icon und sofortigen Start zu erhalten.',
    'Keep this Cardputer on the same Wi-Fi as this device.': 'Halte diesen Cardputer im selben WLAN wie dieses Gerät.',
    'Desktop (Chrome/Edge): click the ⊕ / install icon in the address bar.': 'Desktop (Chrome/Edge): Klicke auf das ⊕ / Installieren-Symbol in der Adressleiste.',
    'iPhone/iPad (Safari): Share → “Add to Home Screen”.': 'iPhone/iPad (Safari): Teilen → “Zum Home-Bildschirm”.',
    'Android (Chrome): menu ⋮ → “Install app”.': 'Android (Chrome): Menü ⋮ → “App installieren”.',
    '💡 Tip: bookmark the device IP so you can always get back to NucleoOS.': '💡 Tipp: Setze ein Lesezeichen auf die Geräte-IP, um immer zu NucleoOS zurückzufinden.',
    'Install NucleoOS': 'NucleoOS installieren',
    'Add it like a native app': 'Füge es wie eine native App hinzu',
    '⤓ Install app': '⤓ App installieren',
    'RECOMMENDED': 'EMPFOHLEN',
    'Choose your AI': 'Wähle deine KI',
    'You can change this anytime in Settings ▸ AI': 'Du kannst dies jederzeit unter Einstellungen ▸ KI ändern',
    'Best understanding. Runs in your browser — zero load on the Cardputer.': 'Bestes Verständnis. Läuft in deinem Browser — keine Last für den Cardputer.',
    'Fast & free tier. OpenAI-compatible (Llama models).': 'Schnell und mit Gratis-Kontingent. OpenAI-kompatibel (Llama-Modelle).',
    'Strong, generous free tier. Routed via the device proxy (no browser CORS).': 'Stark, großzügiges Gratis-Kontingent. Über den Geräte-Proxy geleitet (kein Browser-CORS).',
    'Offline only': 'Nur offline',
    'No key. ANIMA still answers from its on-device brain, weather & Wikipedia.': 'Kein Schlüssel. ANIMA antwortet weiterhin aus ihrem Gehirn auf dem Gerät, Wetter und Wikipedia.',
    'Paste a key first.': 'Füge zuerst einen Schlüssel ein.',
    'testing…': 'teste…',
    '✓ Key works!': '✓ Schlüssel funktioniert!',
    '✗ Key rejected.': '✗ Schlüssel abgelehnt.',
    '⚡ Test': '⚡ Testen',
    'Paste your key.': 'Füge deinen Schlüssel ein.',
    'That doesn’t look like a ': 'Das sieht nicht nach einem ',
    ' key. Save anyway?': '-Schlüssel aus. Trotzdem speichern?',
    'saving…': 'speichere…',
    'detecting plan…': 'erkenne Tarif…',
    '✓ Saved — ANIMA is ready.': '✓ Gespeichert — ANIMA ist bereit.',
    'Pair this browser first.': 'Koppele zuerst diesen Browser.',
    'Save failed.': 'Speichern fehlgeschlagen.',
    '💾 Save & finish': '💾 Speichern und fertig',
    'Go to ': 'Gehe zu ',
    'Open API Keys → Create Key.': 'Öffne API Keys → Create Key.',
    'Copy the key (starts with sk-ant-) and paste it below.': 'Kopiere den Schlüssel (beginnt mit sk-ant-) und füge ihn unten ein.',
    'Create an API key (starts with AIza) and paste it below.': 'Erstelle einen API-Schlüssel (beginnt mit AIza) und füge ihn unten ein.',
    'The Cardputer relays the call (Google has no browser access).': 'Der Cardputer leitet den Aufruf weiter (Google hat keinen Browser-Zugriff).',
    'Create an API key (starts with gsk_) and paste it below.': 'Erstelle einen API-Schlüssel (beginnt mit gsk_) und füge ihn unten ein.',
    'Connect ': 'Verbinde ',
    'Your key stays on the device': 'Dein Schlüssel bleibt auf dem Gerät',
    'Model': 'Modell',
    'Stored only on your device (SD), never logged. With Claude the key stays in <b>your browser</b> and talks directly to Anthropic — the Cardputer isn’t loaded.': 'Nur auf deinem Gerät (SD) gespeichert, nie protokolliert. Mit Claude bleibt der Schlüssel in <b>deinem Browser</b> und spricht direkt mit Anthropic — der Cardputer wird nicht belastet.',
    'Skip AI setup? You can do it later in Settings ▸ AI.': 'KI-Einrichtung überspringen? Du kannst sie später unter Einstellungen ▸ KI vornehmen.',
    'Set up later': 'Später',
    '← Back': '← Zurück',
    'Next →': 'Weiter →',
    'Start using NucleoOS': 'NucleoOS nutzen',
  },
};
const TR = (it, en) => { const l = lang(); return l === 'it' ? it : l === 'en' ? en : (L10N[l] && L10N[l][en] != null ? L10N[l][en] : en); };

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
        || [{ code: 'it', label: 'Italiano', flag: '🇮🇹' }, { code: 'en', label: 'English', flag: '🇬🇧' },
            { code: 'es', label: 'Español', flag: '🇪🇸' }, { code: 'fr', label: 'Français', flag: '🇫🇷' }, { code: 'de', label: 'Deutsch', flag: '🇩🇪' }];
      const cur = lang();
      const pick = (code) => {
        if (window.NucleoI18N) window.NucleoI18N.setLang(code);
        else { try { localStorage.setItem('anima.lang', code); } catch {} }
        render();
      };
      card.append(
        header(TR('Scegli la lingua', 'Choose your language'),
               TR('Puoi cambiarla quando vuoi in Impostazioni ▸ Aspetto', 'You can change it anytime in Settings ▸ Appearance')),
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
        header(TR('Benvenuto in NucleoOS', 'Welcome to NucleoOS'),
               TR('Un OS web-native che vive sul tuo Cardputer', 'A web-native OS that lives on your Cardputer')),
        h('div', { class: 'nob-body' },
          h('p', { html: TR(
            'NucleoOS trasforma il tuo M5Stack Cardputer in un piccolo computer che guidi da questo browser. Al centro c’è <b>ANIMA</b> — un assistente il cui cervello gira <b>offline, sul dispositivo stesso</b>.',
            'NucleoOS turns your M5Stack Cardputer into a tiny computer you drive from this browser. At its heart is <b>ANIMA</b> — an assistant whose brain runs <b>offline, on the device itself</b>.') }),
          h('div', { class: 'nob-feat' },
            feat('🧠', TR('Cervello offline', 'Offline brain'), TR('Risposte, ragionamento e memoria — senza cloud.', 'Answers, reasoning and memory — no cloud needed.')),
            feat('☁', TR('Cloud opzionale', 'Optional cloud'), TR('Aggiungi una chiave Claude per comprensione e verifica migliori.', 'Add a Claude key for deeper understanding & verification.')),
            feat('🔒', TR('Le tue chiavi, il tuo device', 'Your keys, your device'), TR('Le chiavi restano sulla SD; le chiamate cloud partono dal tuo browser.', 'Keys stay on the SD; cloud calls go straight from your browser.')),
            feat('⚡', TR('Leggero sul Cardputer', 'Light on the Cardputer'), TR('Il lavoro pesante dell’IA gira nel browser, non sul chip.', 'The heavy AI work runs in your browser, not the chip.'))),
          h('p', { class: 'nob-note', html: TR('✨ Due minuti adesso e ANIMA è pronta ovunque nell’OS.', '✨ Two minutes now and ANIMA is ready everywhere in the OS.') })),
        footer({ next: true, skip: true }));
    }
    function feat(ic, t, d) {
      return h('div', null, h('div', { class: 'ic' }, ic), h('div', { class: 't' }, t), h('div', { class: 'd' }, d));
    }

    // ---- step 1: install NucleoOS -------------------------------------------
    function renderInstall() {
      const prompt = window.__nucleoInstallPrompt;
      const body = h('div', { class: 'nob-body' },
        h('p', { html: TR(
          'NucleoOS è servito dal Cardputer sulla tua rete — niente app store. <b>Installalo</b> per avere un’icona a tutto schermo e avvio immediato.',
          'NucleoOS is served by the Cardputer over your network — no app store. <b>Install it</b> to get a full-screen app icon and instant launch.') }),
        h('ol', { class: 'nob-steps' },
          li(TR('Tieni il Cardputer sulla stessa Wi-Fi di questo dispositivo.', 'Keep this Cardputer on the same Wi-Fi as this device.')),
          li(TR('Desktop (Chrome/Edge): clicca l’icona ⊕ / installa nella barra indirizzi.', 'Desktop (Chrome/Edge): click the ⊕ / install icon in the address bar.')),
          li(TR('iPhone/iPad (Safari): Condividi → “Aggiungi a Home”.', 'iPhone/iPad (Safari): Share → “Add to Home Screen”.')),
          li(TR('Android (Chrome): menu ⋮ → “Installa app”.', 'Android (Chrome): menu ⋮ → “Install app”.'))),
        h('p', { class: 'nob-note', html: TR('💡 Suggerimento: salva l’IP del dispositivo per tornare sempre a NucleoOS.', '💡 Tip: bookmark the device IP so you can always get back to NucleoOS.') }));
      card.append(header(TR('Installa NucleoOS', 'Install NucleoOS'), TR('Aggiungilo come un’app nativa', 'Add it like a native app')), body);
      const extra = [];
      if (prompt) extra.push(h('button', { class: 'nob-btn', on: { click: async () => { try { prompt.prompt(); await prompt.userChoice; } catch {} window.__nucleoInstallPrompt = null; } } }, TR('⤓ Installa app', '⤓ Install app')));
      card.append(footer({ back: true, next: true, skip: true, extra }));
    }
    function li(...kids) { return h('li', null, ...kids); }

    // ---- step 2: choose provider --------------------------------------------
    function renderProvider() {
      const mk = (id, ic, title, desc, reco) => h('button', { class: 'nob-choice' + (state.provider === id ? ' sel' : ''), 'data-id': id,
        on: { click: () => { state.provider = id; render(); } } },
        h('div', { class: 'cic' }, ic),
        h('div', null, h('div', { class: 'ct', html: title + (reco ? '<span class="reco">' + TR('CONSIGLIATO', 'RECOMMENDED') + '</span>' : '') }), h('div', { class: 'cd' }, desc)));
      card.append(
        header(TR('Scegli la tua IA', 'Choose your AI'), TR('Puoi cambiarlo quando vuoi in Impostazioni ▸ IA', 'You can change this anytime in Settings ▸ AI')),
        h('div', { class: 'nob-body' }, h('div', { class: 'nob-choices' },
          mk('anthropic', '🟣', 'Claude (Anthropic)', TR('Comprensione al top. Gira nel browser — zero carico sul Cardputer.', 'Best understanding. Runs in your browser — zero load on the Cardputer.'), true),
          mk('openai', '⚡', 'Groq', TR('Veloce e con piano gratuito. OpenAI-compatibile (modelli Llama).', 'Fast & free tier. OpenAI-compatible (Llama models).'), false),
          mk('google', '✨', 'Gemini (Google)', TR('Forte, free tier generoso. Instradato dal proxy del device (niente CORS browser).', 'Strong, generous free tier. Routed via the device proxy (no browser CORS).'), false),
          mk('offline', '🧠', TR('Solo offline', 'Offline only'), TR('Nessuna chiave. ANIMA risponde comunque dal cervello on-device, meteo e Wikipedia.', 'No key. ANIMA still answers from its on-device brain, weather & Wikipedia.'), false))),
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
        const k = input.value.trim(); if (!k) { setStat(TR('Incolla prima una chiave.', 'Paste a key first.'), 'err'); return; }
        setStat(TR('provo…', 'testing…')); testBtn.disabled = true;
        try { const ok = await AI.cloudPing(cfg()); setStat(ok ? TR('✓ La chiave funziona!', '✓ Key works!') : TR('✗ Chiave rifiutata.', '✗ Key rejected.'), ok ? 'ok' : 'err'); }
        catch (e) { setStat('✗ ' + String(e.message || e), 'err'); }
        finally { testBtn.disabled = false; }
      } } }, TR('⚡ Prova', '⚡ Test'));

      const saveBtn = h('button', { class: 'nob-btn pri', on: { click: async () => {
        const k = input.value.trim();
        if (!k) { setStat(TR('Incolla la tua chiave.', 'Paste your key.'), 'err'); return; }
        if (!p.prefix.test(k) && !confirm(TR('Non sembra una chiave ', 'That doesn’t look like a ') + p.label + TR('. Salvo lo stesso?', ' key. Save anyway?'))) return;
        setStat(TR('salvo…', 'saving…')); saveBtn.disabled = true;
        const res = await AI.writeTeacher(cfg());
        saveBtn.disabled = false;
        if (res === true && state.provider === 'google') {
          // Detect the Gemini plan (real models + paid/free tier) and re-save with the recommended model + tier.
          // Best-effort: if it can't reach the API it just keeps the Flash default and finishes anyway.
          setStat(TR('rilevo il piano…', 'detecting plan…'));
          try {
            const c = await AI.calibrateGemini({ base: p.base, key: k });
            const d = cfg(); if (!c.models.includes(d.model)) d.model = c.recommended; d.geminiTier = c.tier;
            await AI.writeTeacher(d);
            setStat('✓ ' + AI.geminiTierLabel(c.tier, lang()), 'ok');
          } catch { setStat(TR('✓ Salvata — ANIMA è pronta.', '✓ Saved — ANIMA is ready.'), 'ok'); }
          setTimeout(done, 1000); return;
        }
        if (res === true) { setStat(TR('✓ Salvata — ANIMA è pronta.', '✓ Saved — ANIMA is ready.'), 'ok'); setTimeout(done, 650); }
        else if (res === 'unpaired') setStat(TR('Accoppia prima il browser.', 'Pair this browser first.'), 'err');
        else setStat(TR('Salvataggio fallito.', 'Save failed.'), 'err');
      } } }, TR('💾 Salva e finisci', '💾 Save & finish'));

      const where = state.provider === 'anthropic'
        ? [li(TR('Vai su ', 'Go to '), h('a', { href: 'https://console.anthropic.com/settings/keys', target: '_blank', rel: 'noopener' }, 'console.anthropic.com')),
           li(TR('Apri API Keys → Create Key.', 'Open API Keys → Create Key.')),
           li(TR('Copia la chiave (inizia con sk-ant-) e incollala qui sotto.', 'Copy the key (starts with sk-ant-) and paste it below.'))]
        : state.provider === 'google'
        ? [li(TR('Vai su ', 'Go to '), h('a', { href: 'https://aistudio.google.com/apikey', target: '_blank', rel: 'noopener' }, 'aistudio.google.com/apikey')),
           li(TR('Crea una API key (inizia con AIza) e incollala qui sotto.', 'Create an API key (starts with AIza) and paste it below.')),
           li(TR('Il Cardputer inoltra la chiamata (Google non è accessibile dal browser).', 'The Cardputer relays the call (Google has no browser access).'))]
        : [li(TR('Vai su ', 'Go to '), h('a', { href: 'https://console.groq.com/keys', target: '_blank', rel: 'noopener' }, 'console.groq.com')),
           li(TR('Crea una API key (inizia con gsk_) e incollala qui sotto.', 'Create an API key (starts with gsk_) and paste it below.'))];

      input.addEventListener('keydown', (e) => { if (e.key === 'Enter') { e.preventDefault(); saveBtn.click(); } });

      card.append(
        header(TR('Collega ', 'Connect ') + p.label, TR('La chiave resta sul dispositivo', 'Your key stays on the device')),
        h('div', { class: 'nob-body' },
          h('ol', { class: 'nob-steps' }, ...where),
          h('div', { class: 'nob-field' },
            h('label', null, TR('Modello', 'Model')), sel,
            h('label', null, 'API key'), input,
            h('div', { class: 'nob-row' }, testBtn), stat),
          h('div', { class: 'nob-note', html: '🔒 ' + TR(
            'Salvata solo sul tuo dispositivo (SD), mai nei log. Con Claude la chiave resta nel <b>tuo browser</b> e parla direttamente con Anthropic — il Cardputer non viene caricato.',
            'Stored only on your device (SD), never logged. With Claude the key stays in <b>your browser</b> and talks directly to Anthropic — the Cardputer isn’t loaded.') })),
        footer({ back: true, saveSlot: saveBtn }));
    }

    // ---- footer --------------------------------------------------------------
    function footer(opts) {
      const btns = h('div', { class: 'nob-btns' });
      if (opts.extra) for (const e of opts.extra) btns.append(e);
      if (opts.skip) btns.append(h('button', { class: 'nob-btn ghost', on: { click: () => { if (confirm(TR('Salto la configurazione IA? Puoi farla dopo in Impostazioni ▸ IA.', 'Skip AI setup? You can do it later in Settings ▸ AI.'))) done(); } } }, TR('Più tardi', 'Set up later')));
      if (opts.back) btns.append(h('button', { class: 'nob-btn', on: { click: () => { state.step = Math.max(0, state.step - 1); render(); } } }, TR('← Indietro', '← Back')));
      if (opts.saveSlot) btns.append(opts.saveSlot);
      if (opts.next) btns.append(h('button', { class: 'nob-btn pri', on: { click: () => { state.step++; render(); } } }, TR('Avanti →', 'Next →')));
      if (opts.finish) btns.append(h('button', { class: 'nob-btn pri', on: { click: done } }, TR('Inizia a usare NucleoOS', 'Start using NucleoOS')));
      return h('div', { class: 'nob-ft' }, dots(), btns);
    }

    render();
    document.body.append(scrim);
  });
}
