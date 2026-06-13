// i18n.js — tiny bilingual string table for the Settings app (IT primary, EN aware).
//
// Mirrors the rest of NucleoOS: Italian is the default UI language; English is offered and the
// system language (ANIMA / copilot / voice) follows the same `ui.language` setting. `t(lang,key)`
// resolves against the active language, falling back to IT then the raw key so a missing string is
// never a blank in the UI. Pure (no DOM) so it can be unit-tested.

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
