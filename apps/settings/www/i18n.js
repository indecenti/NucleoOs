// i18n.js — bilingual string table for the Settings app (IT primary, EN aware).
//
// Mirrors the rest of NucleoOS: Italian is the default UI language; English is offered and the
// system language (ANIMA / copilot / voice) follows the same `ui.language` setting. `t(lang,key)`
// resolves against the active language, falling back to IT then the raw key so a missing string is
// never a blank in the UI. Pure (no DOM) so it can be unit-tested.
//
// The Settings markup is bound to these keys declaratively via `data-t` / `data-t-html` /
// `data-t-attr` annotations (applied by applyStaticI18n() in index.html) plus imperative T() calls
// for JS-built strings. Keep the `it` and `en` blocks in key parity.

export const STR = {
  it: {
    appTitle: 'Impostazioni', appSub: 'Controllo completo del dispositivo',
    // tabs
    tab_control: 'Centro di Controllo', tab_display: 'Schermo', tab_personalization: 'Aspetto',
    tab_network: 'Rete', tab_ai: 'IA / Modelli', tab_device: 'Dispositivo',
    tab_diagnostics: 'Diagnostica', tab_about: 'Info',
    // control center
    pulse: 'Battito del dispositivo', heap: 'Memoria libera', frag: 'Frammentazione',
    cpu: 'CPU', signal: 'Segnale Wi-Fi', uptime: 'Acceso da', battery: 'Batteria',
    scenes: 'Scene', quickToggles: 'Interruttori rapidi', screen: 'Schermo',
    voiceAlways: 'Voce sempre attiva', offlineBrain: 'Cervello offline', tts: 'Voce on-device',
    brightness: 'Luminosità', volume: 'Volume', pollRate: 'Frequenza aggiornamento',
    frozen: 'In pausa', live: 'Live', freeze: 'Pausa', resume: 'Riprendi',
    lastUpdate: 'aggiornato', secondsAgo: 's fa', online: 'Online', offline: 'Offline',
    busy: 'Occupato', heapTight: 'Memoria al limite', applyScene: 'Applica scena',
    // common
    on: 'Acceso', off: 'Spento', auto: 'Auto', save: 'Salva', reload: 'Ricarica',
    ready: 'Pronto', unreachable: 'Dispositivo non raggiungibile', saved: 'Salvato sul dispositivo',
    cmdPlaceholder: 'Comanda il Cardputer… (prova "spegni schermo", "riavvia", "luminosità")',
    cmdHint: 'Ctrl+K per il comando rapido', noMatch: 'Nessuna corrispondenza',
    selfTest: 'Self-Test', runSelfTest: 'Esegui Self-Test', testing: 'Verifica in corso…',
    pass: 'OK', fail: 'Errore', warn: 'Attenzione', copyReport: 'Copia esito', copied: 'Copiato',
    reboot: 'Riavvia', rebootConfirm: 'Confermi il riavvio?', rebooting: 'Riavvio…',
    scan: 'Scansiona', scanning: 'Scansione…', stored: 'salvato', appliedAtBoot: 'applicato al riavvio',
    langHint: 'Imposta la lingua di sistema per ANIMA, il copilota e la voce. Si applica subito a tutto l’OS.',
    langRegion: 'Lingua e regione', displayLang: 'Lingua di visualizzazione', regionFormat: 'Formato regionale',
    regionAuto: 'Automatico (segue la lingua)',
    pvDate: 'Data', pvTime: 'Ora', pvNumber: 'Numeri', pvRelative: 'Tempo relativo',
    langStatusOk: 'Interfaccia tradotta — {n} superfici, {k} stringhe, {langs} sincronizzate.',

    // ── shared badges / generic labels ──
    badgeOneTap: 'Un tocco', badgePulse: 'Battito', badgeReadOnly: 'Sola lettura',
    badgeSmart: 'Intelligente', badgeHowItWorks: 'Come funziona', badgeNote: 'Nota',
    theme: 'Tema', state: 'Stato', policy: 'Politica', always: 'Sempre', never: 'Mai',
    cancel: 'Annulla', apply: 'Applica', profile: 'Profilo', run: 'Esegui', refresh: 'Aggiorna',
    clear: 'Svuota', na: 'n/d', used: 'usati', free: 'libere', cmdBar: 'Comando rapido', storage: 'Storage',
    // control-center tiles
    fragShort: 'Framm.', minHist: 'Min storico',
    // display
    dispState: 'Stato schermo', turnOn: 'Accendi', turnOff: 'Spegni',
    screenOnState: 'Schermo ACCESO (ultima azione).', screenOffState: 'Schermo SPENTO.',
    dispHint: 'Lo spegnimento porta la retroilluminazione a 0 (nero) senza cambiare la luminosità impostata. È l’<b>unica leva live</b> dello schermo via web; non interroga il pannello all’apertura per non risvegliarlo.',
    briAudio: 'Luminosità & Audio', briDisplay: 'Luminosità display', volAudio: 'Volume audio',
    briVolHint: 'Luminosità e volume si <b>salvano</b> nelle impostazioni e si applicano al <b>prossimo avvio</b> (il firmware non espone un setter a caldo per questi).',
    // appearance
    themeLight: 'Chiaro', themeDark: 'Scuro', accentColor: 'Colore accento', textSize: 'Dimensione testo',
    sizeSmall: 'Piccolo', sizeNormal: 'Normale', sizeLarge: 'Grande',
    wallpaper: 'Sfondo', wpLoadHint: 'Apri questa scheda per caricare gli sfondi…',
    wpEmpty: 'Nessuno sfondo in /data/Pictures/',
    // network
    wifiStatus: 'Stato Wi-Fi', netMode: 'Modalità', ipAddr: 'Indirizzo IP', clock: 'Orologio',
    nearbyNets: 'Reti vicine',
    wifiScanHint: 'La scansione è <b>su richiesta</b> e a sola lettura. Per <b>connettersi</b> o cambiare rete usa l’app <b>Wi-Fi</b> nativa: il firmware non espone un endpoint di join via web.',
    connectivity: 'Connettività', changesNextBoot: 'Le modifiche si salvano e si applicano al prossimo avvio.',
    enableIpv6: 'Abilita IPv6',
    ipv6Hint: 'IPv6 è <b>disabilitato</b> nel firmware per liberare SRAM: tutta la rete viaggia su IPv4. Le tabelle IPv6 di lwIP si allocano a compilazione — questo controllo registra solo la preferenza; per riattivarlo davvero ricompila con <code>CONFIG_LWIP_IPV6=y</code>.',
    hiddenNet: '(nascosta)', connectedF: 'connessa',
    // AI
    aiProfile: 'Profilo IA',
    aiProfileHint: 'Scegli <b>come deve ragionare ANIMA</b>. Il sistema rileva cosa è davvero disponibile — chiavi online, GPU del browser, cervello offline — e <b>attiva solo i profili possibili</b>, spiegando cosa manca.',
    detectingOptions: 'rilevo le possibilità…', whatCurrentCanDo: 'Cosa può fare la configurazione attuale',
    whichApps: 'Su quali app agisce', advSettings: 'Impostazioni avanzate · provider, chiave, cervello offline, voce',
    brainsChain: 'Cervelli & catena di fallback',
    brainsChainHint: 'ANIMA ha <b>quattro menti</b> e le prova <b>in ordine</b>, dalla più capace a quella sempre-disponibile. Sfrutta prima la potenza del browser/PC e <b>scala sul Cardputer solo come ultima rete di sicurezza</b>.',
    brain1Title: '1 · IA online (cloud)', brain1Desc: 'Claude / Groq / Grok — serve chiave + Internet. La più capace.',
    brain2Title: '2 · LLM locale (GPU del browser)', brain2Desc: 'Un LLM completo sulla GPU del browser (WebLLM). Offline dopo il primo download.',
    brain3Title: '3 · Browser — cervello locale (WASM) + indice web', brain3Desc: 'La cascata del Cardputer compilata a WebAssembly, sulla CPU del browser. <b>Ultimo passo prima del Cardputer.</b>',
    brain4Title: '4 · Cardputer (on-device)', brain4Desc: 'Il cervello nel dispositivo. <b>Sempre disponibile e offline</b>: la rete di sicurezza finale.',
    offlineBrainL1: 'Cervello offline (L1)',
    l1PolicyHint: 'In <b>Auto</b> il cervello offline si fa da parte (liberando ~31&nbsp;KB) quando c’è un cervello più forte (online o LLM browser). <b>Sempre</b> lo tiene attivo, <b>Mai</b> non lo serve mai.',
    aiAssistant: 'Assistente IA · Provider e chiave', loadingKeyMgr: 'carico il gestore chiavi…',
    aiKeysHint: '🎨 <b>Generare immagini con l’IA</b> in Paint (Atelier) richiede una chiave <b>Grok (xAI)</b> — modello <code>grok-2-image</code>. Claude e Groq non disegnano, ma possono <b>migliorare il prompt</b>. 🎙️ La <b>trascrizione vocale</b> (Recorder, Dettatura) usa sempre Groq/OpenAI (Whisper), anche con Claude, Gemini o Grok come chat.',
    browserWasm: 'Browser · cervello locale (WASM)', checking: 'controllo…', prepareUpdate: 'Prepara / Aggiorna',
    webSearchOnDemand: 'Ricerca web on-demand',
    edgeWebHint: 'In modalità <b>Browser</b>, quando i cervelli locali non sanno, recupera la risposta <b>certa</b> da Wikipedia (dal browser, mai dal Cardputer) e la <b>mette in cache</b>: da lì resta offline. Spento = solo conoscenza già in cache (nulla esce dal dispositivo).',
    cachedKnowledge: 'Conoscenza in cache',
    edgeBrainHint: 'Lo <b>stesso motore del Cardputer</b> compilato a WebAssembly: apre app, calcola, risponde e impara <b>senza che nulla lasci il dispositivo</b>. Si scarica una volta (~14&nbsp;MB), poi funziona anche col Cardputer <b>spento</b>. Con la ricerca web on-demand diventa un <b>centralizzatore di conoscenza certa</b> che cresce con l’uso.',
    speakWhenAsked: 'Parla quando interrogato', readingSpeed: 'Velocità di lettura', testVoice: 'Prova voce',
    voiceAlwaysHint: 'Tiene i <b>comandi vocali</b> (PTT col tasto FN) sempre pronti dalla home. Costa ~16&nbsp;KB: lascialo spento finché non hai registrato i comandi.',
    // device
    deviceIdentity: 'Identità dispositivo', deviceName: 'Nome dispositivo', timezone: 'Fuso orario',
    deviceNameHint: 'Il nome è usato da mDNS/Swarm e si applica al prossimo avvio.',
    maintenance: 'Manutenzione', rebootDevice: 'Riavvia dispositivo',
    maintenanceHint: 'Il riavvio ricarica firmware e indice ANIMA dalla SD. Per gli aggiornamenti firmware usa l’app <b>Aggiornamenti</b> (l’OTA è inaffidabile su questo dispositivo: preferisci il flash seriale).',
    // diagnostics
    selfTestEndpoints: 'Self-Test endpoint', heapRegions: 'Heap (regioni)',
    heapOpenHint: 'Apri questa scheda per leggere l’heap…', systemLogs: 'Log di sistema',
    logsHint: 'Ring RAM ~2&nbsp;KB · funziona anche senza SD/seriale.', privacy: 'Privacy',
    privacyHint: 'Alcuni endpoint diagnostici (<code>/api/status</code>, <code>/api/heap</code>, <code>/api/cpu</code>, <code>/api/logs</code>) sono <b>pubblici</b> sulla rete locale e mostrano SSID/IP, memoria e cause di reboot. Tienilo presente su reti condivise.',
    heapInternalFree: 'Internal libera', heapMaxBlock: 'Blocco max', heapDmaFree: 'DMA libera',
    heapHttpdStack: 'Stack httpd min', emptyParen: '(vuoto)',
    // about
    systemInfo: 'Informazioni sistema', platform: 'Piattaforma', sdStorage: 'Storage SD',
    installedApps: 'App installate', controlPanel: 'Pannello di Controllo',
    aboutHint: 'Versione app <b>1.0</b> · Centro di Controllo con Battito live, scene, comando rapido e self-test. Pensato per dare <b>controllo completo da web</b> senza mai sovraccaricare il Cardputer.',
    // modals
    rebootTitle: 'Conferma riavvio', rebootBody: 'Vuoi davvero riavviare il dispositivo? Le connessioni attive verranno interrotte.',
    // dynamic status / messages
    notMounted: 'non montato', applyingScene: 'Applico scena: ', sceneApplied: 'Scena applicata: ',
    screenOnMsg: 'Schermo acceso', screenOffMsg: 'Schermo spento',
    voiceOnState: 'attiva (PTT da home)', voiceOffState: 'su richiesta',
    voiceAlwaysMsg: 'Voce sempre attiva', voiceOnDemandMsg: 'Voce su richiesta',
    offlineBrainMsg: 'Cervello offline: ', cmdError: 'Errore comando', fragmentation: 'Frammentazione',
    saveFailed: 'Salvataggio fallito', saving: 'Salvataggio…', personalizationSaved: 'Personalizzazione salvata',
    rebootDoneMsg: 'Dispositivo in riavvio — ricarica la pagina tra qualche secondo',
    l1Serving: 'attivo (sta servendo richieste offline)', l1Off: 'spento (RAM liberata)',
    l1Idle: 'a riposo (un cervello più forte è attivo)',
    ttsUnavailable: 'pacchetto voce non installato (/data/tts/)', ttsOn: 'attiva', ttsOff: 'disattivata',
    playing: 'riproduco…', keyMgrUnavailable: 'Gestore chiavi non disponibile',
    edgeCached: '✓ in cache · pronto offline', edgeNotCached: '○ non ancora scaricato — premi "Prepara"',
    edgeUnreachable: 'motore non raggiungibile da qui', preparing: 'preparo…', downloading: 'scarico…',
    readyCheck: '✓ pronto', kbEntries: 'voci di conoscenza web · pronte offline',
    kbEmpty: 'nessuna voce ancora (cresce con l’uso online)', kbClearConfirm: 'Svuotare la conoscenza web in cache?',
    loading: 'Caricamento…',
  },
  en: {
    appTitle: 'Settings', appSub: 'Full device control',
    tab_control: 'Control Center', tab_display: 'Display', tab_personalization: 'Appearance',
    tab_network: 'Network', tab_ai: 'AI / Models', tab_device: 'Device',
    tab_diagnostics: 'Diagnostics', tab_about: 'About',
    pulse: 'Device pulse', heap: 'Free memory', frag: 'Fragmentation',
    cpu: 'CPU', signal: 'Wi-Fi signal', uptime: 'Uptime', battery: 'Battery',
    scenes: 'Scenes', quickToggles: 'Quick toggles', screen: 'Screen',
    voiceAlways: 'Voice always on', offlineBrain: 'Offline brain', tts: 'On-device voice',
    brightness: 'Brightness', volume: 'Volume', pollRate: 'Refresh rate',
    frozen: 'Paused', live: 'Live', freeze: 'Pause', resume: 'Resume',
    lastUpdate: 'updated', secondsAgo: 's ago', online: 'Online', offline: 'Offline',
    busy: 'Busy', heapTight: 'Memory tight', applyScene: 'Apply scene',
    on: 'On', off: 'Off', auto: 'Auto', save: 'Save', reload: 'Reload',
    ready: 'Ready', unreachable: 'Device unreachable', saved: 'Saved to device',
    cmdPlaceholder: 'Command the Cardputer… (try "screen off", "reboot", "brightness")',
    cmdHint: 'Ctrl+K for the command bar', noMatch: 'No match',
    selfTest: 'Self-Test', runSelfTest: 'Run Self-Test', testing: 'Testing…',
    pass: 'OK', fail: 'Fail', warn: 'Warning', copyReport: 'Copy report', copied: 'Copied',
    reboot: 'Reboot', rebootConfirm: 'Confirm reboot?', rebooting: 'Rebooting…',
    scan: 'Scan', scanning: 'Scanning…', stored: 'stored', appliedAtBoot: 'applied at next boot',
    langHint: 'Set the system language for ANIMA, the copilot and voice. Applies instantly across the OS.',
    langRegion: 'Language & region', displayLang: 'Display language', regionFormat: 'Regional format',
    regionAuto: 'Automatic (follow language)',
    pvDate: 'Date', pvTime: 'Time', pvNumber: 'Numbers', pvRelative: 'Relative time',
    langStatusOk: 'Interface translated — {n} surfaces, {k} strings, {langs} in sync.',

    // ── shared badges / generic labels ──
    badgeOneTap: 'One tap', badgePulse: 'Pulse', badgeReadOnly: 'Read-only',
    badgeSmart: 'Smart', badgeHowItWorks: 'How it works', badgeNote: 'Note',
    theme: 'Theme', state: 'Status', policy: 'Policy', always: 'Always', never: 'Never',
    cancel: 'Cancel', apply: 'Apply', profile: 'Profile', run: 'Run', refresh: 'Refresh',
    clear: 'Clear', na: 'n/a', used: 'used', free: 'free', cmdBar: 'Command bar', storage: 'Storage',
    // control-center tiles
    fragShort: 'Frag.', minHist: 'Lowest ever',
    // display
    dispState: 'Screen state', turnOn: 'Turn on', turnOff: 'Turn off',
    screenOnState: 'Screen ON (last action).', screenOffState: 'Screen OFF.',
    dispHint: 'Turning it off drives the backlight to 0 (black) without changing the set brightness. It is the <b>only live lever</b> for the screen over the web; it does not query the panel on open, to avoid waking it.',
    briAudio: 'Brightness & Audio', briDisplay: 'Display brightness', volAudio: 'Audio volume',
    briVolHint: 'Brightness and volume are <b>saved</b> in settings and applied at the <b>next boot</b> (the firmware exposes no hot setter for these).',
    // appearance
    themeLight: 'Light', themeDark: 'Dark', accentColor: 'Accent color', textSize: 'Text size',
    sizeSmall: 'Small', sizeNormal: 'Normal', sizeLarge: 'Large',
    wallpaper: 'Wallpaper', wpLoadHint: 'Open this tab to load wallpapers…',
    wpEmpty: 'No wallpapers in /data/Pictures/',
    // network
    wifiStatus: 'Wi-Fi status', netMode: 'Mode', ipAddr: 'IP address', clock: 'Clock',
    nearbyNets: 'Nearby networks',
    wifiScanHint: 'The scan is <b>on demand</b> and read-only. To <b>connect</b> or change network use the native <b>Wi-Fi</b> app: the firmware exposes no join endpoint over the web.',
    connectivity: 'Connectivity', changesNextBoot: 'Changes are saved and applied at next boot.',
    enableIpv6: 'Enable IPv6',
    ipv6Hint: 'IPv6 is <b>disabled</b> in the firmware to free SRAM: all networking runs over IPv4. lwIP’s IPv6 tables are allocated at compile time — this control only records the preference; to truly re-enable it, rebuild with <code>CONFIG_LWIP_IPV6=y</code>.',
    hiddenNet: '(hidden)', connectedF: 'connected',
    // AI
    aiProfile: 'AI profile',
    aiProfileHint: 'Choose <b>how ANIMA should reason</b>. The system detects what is actually available — online keys, browser GPU, offline brain — and <b>enables only the feasible profiles</b>, explaining what is missing.',
    detectingOptions: 'detecting options…', whatCurrentCanDo: 'What the current setup can do',
    whichApps: 'Which apps it affects', advSettings: 'Advanced settings · provider, key, offline brain, voice',
    brainsChain: 'Brains & fallback chain',
    brainsChainHint: 'ANIMA has <b>four minds</b> and tries them <b>in order</b>, from the most capable to the always-available one. It leans on browser/PC power first and <b>scales down to the Cardputer only as the final safety net</b>.',
    brain1Title: '1 · Online AI (cloud)', brain1Desc: 'Claude / Groq / Grok — needs a key + Internet. The most capable.',
    brain2Title: '2 · Local LLM (browser GPU)', brain2Desc: 'A full LLM on the browser GPU (WebLLM). Offline after the first download.',
    brain3Title: '3 · Browser — local brain (WASM) + web index', brain3Desc: 'The Cardputer cascade compiled to WebAssembly, on the browser CPU. <b>Last step before the Cardputer.</b>',
    brain4Title: '4 · Cardputer (on-device)', brain4Desc: 'The brain in the device. <b>Always available and offline</b>: the final safety net.',
    offlineBrainL1: 'Offline brain (L1)',
    l1PolicyHint: 'In <b>Auto</b> the offline brain stands aside (freeing ~31&nbsp;KB) when a stronger brain is available (online or browser LLM). <b>Always</b> keeps it resident, <b>Never</b> never serves it.',
    aiAssistant: 'AI assistant · Provider & key', loadingKeyMgr: 'loading the key manager…',
    aiKeysHint: '🎨 <b>Generating images with AI</b> in Paint (Atelier) requires a <b>Grok (xAI)</b> key — model <code>grok-2-image</code>. Claude and Groq do not draw, but they can <b>improve the prompt</b>. 🎙️ <b>Voice transcription</b> (Recorder, Dictation) always uses Groq/OpenAI (Whisper), even with Claude, Gemini or Grok as chat.',
    browserWasm: 'Browser · local brain (WASM)', checking: 'checking…', prepareUpdate: 'Prepare / Update',
    webSearchOnDemand: 'On-demand web search',
    edgeWebHint: 'In <b>Browser</b> mode, when the local brains do not know, it fetches the <b>certain</b> answer from Wikipedia (from the browser, never from the Cardputer) and <b>caches</b> it: from then on it stays offline. Off = only already-cached knowledge (nothing leaves the device).',
    cachedKnowledge: 'Cached knowledge',
    edgeBrainHint: 'The <b>same Cardputer engine</b> compiled to WebAssembly: it opens apps, computes, answers and learns <b>without anything leaving the device</b>. It downloads once (~14&nbsp;MB), then works even with the Cardputer <b>off</b>. With on-demand web search it becomes a <b>hub of certain knowledge</b> that grows with use.',
    speakWhenAsked: 'Speak when asked', readingSpeed: 'Reading speed', testVoice: 'Test voice',
    voiceAlwaysHint: 'Keeps <b>voice commands</b> (PTT with the FN key) always ready from the home screen. Costs ~16&nbsp;KB: leave it off until you have recorded the commands.',
    // device
    deviceIdentity: 'Device identity', deviceName: 'Device name', timezone: 'Time zone',
    deviceNameHint: 'The name is used by mDNS/Swarm and applies at the next boot.',
    maintenance: 'Maintenance', rebootDevice: 'Reboot device',
    maintenanceHint: 'A reboot reloads the firmware and ANIMA index from SD. For firmware updates use the <b>Updates</b> app (OTA is unreliable on this device: prefer serial flashing).',
    // diagnostics
    selfTestEndpoints: 'Endpoint self-test', heapRegions: 'Heap (regions)',
    heapOpenHint: 'Open this tab to read the heap…', systemLogs: 'System logs',
    logsHint: 'RAM ring ~2&nbsp;KB · works even without SD/serial.', privacy: 'Privacy',
    privacyHint: 'Some diagnostic endpoints (<code>/api/status</code>, <code>/api/heap</code>, <code>/api/cpu</code>, <code>/api/logs</code>) are <b>public</b> on the local network and show SSID/IP, memory and reboot causes. Keep this in mind on shared networks.',
    heapInternalFree: 'Internal free', heapMaxBlock: 'Largest block', heapDmaFree: 'DMA free',
    heapHttpdStack: 'httpd stack min', emptyParen: '(empty)',
    // about
    systemInfo: 'System information', platform: 'Platform', sdStorage: 'SD storage',
    installedApps: 'Installed apps', controlPanel: 'Control Panel',
    aboutHint: 'App version <b>1.0</b> · Control Center with live Pulse, scenes, command bar and self-test. Built to give <b>full control from the web</b> without ever overloading the Cardputer.',
    // modals
    rebootTitle: 'Confirm reboot', rebootBody: 'Do you really want to reboot the device? Active connections will drop.',
    // dynamic status / messages
    notMounted: 'not mounted', applyingScene: 'Applying scene: ', sceneApplied: 'Scene applied: ',
    screenOnMsg: 'Screen on', screenOffMsg: 'Screen off',
    voiceOnState: 'on (PTT from home)', voiceOffState: 'on demand',
    voiceAlwaysMsg: 'Voice always on', voiceOnDemandMsg: 'Voice on demand',
    offlineBrainMsg: 'Offline brain: ', cmdError: 'Command error', fragmentation: 'Fragmentation',
    saveFailed: 'Save failed', saving: 'Saving…', personalizationSaved: 'Personalization saved',
    rebootDoneMsg: 'Device rebooting — reload the page in a few seconds',
    l1Serving: 'active (serving offline requests)', l1Off: 'off (RAM freed)',
    l1Idle: 'idle (a stronger brain is active)',
    ttsUnavailable: 'voice pack not installed (/data/tts/)', ttsOn: 'on', ttsOff: 'off',
    playing: 'playing…', keyMgrUnavailable: 'Key manager unavailable',
    edgeCached: '✓ cached · ready offline', edgeNotCached: '○ not downloaded yet — press "Prepare"',
    edgeUnreachable: 'engine unreachable from here', preparing: 'preparing…', downloading: 'downloading…',
    readyCheck: '✓ ready', kbEntries: 'web knowledge entries · ready offline',
    kbEmpty: 'no entries yet (grows with online use)', kbClearConfirm: 'Clear the cached web knowledge?',
    loading: 'Loading…',
  },
};

export function detectLang(model) {
  const l = model && model.ui && model.ui.language;
  if (l === 'it' || l === 'en') return l;
  const loc = (model && model.device && model.device.locale) || '';
  return /^en/i.test(loc) ? 'en' : 'it';
}

export function t(lang, key) {
  const L = STR[lang] || STR.it;
  return (key in L) ? L[key] : (key in STR.it ? STR.it[key] : key);
}
