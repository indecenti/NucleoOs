/* SPDX-License-Identifier: AGPL-3.0-or-later */
// NucleoOS — shared online-AI key manager (the ONE place the OS asks for / stores a provider key).
//
// Before this module the key UI was copy-pasted into three surfaces (Settings, the ANIMA web app, and
// partly groq-chat), each with its OWN provider table + teacher.json read/write + cloud test — and they
// had already DRIFTED (ANIMA lacked xAI, accepted AQ. Gemini tokens the others rejected; two divergent
// key tests). This component is the single owner of that UI, built entirely on shell/ai.js (the single
// owner of the LOGIC: PROVIDERS/CAPMATRIX/TIERS, teacher.json I/O, cloud calls). Mount it; never re-implement.
//
// Persistence is /data/anima/teacher.json via the paired /api/fs/* — the device-held vault the firmware
// reads. The active provider sits at top-level (what the firmware loads) plus a keys{} map so switching
// Claude↔Groq never loses the other key. The RELEASE system (tools/deploy.ps1, sd-sync.ps1, sd_deploy.py)
// treats that file as device-state and never overwrites it — a key on the SD always wins over the repo.
//
//   import { mountKeyManager } from '/ai-keys.js';
//   const km = mountKeyManager(containerEl, { lang:'it', variant:'full', onChange(cfg){ /* refresh chrome */ } });
//   // handle: km.reload(), km.getCfg(), km.setLang('en'), km.destroy()
//
// opts: variant 'full' (Settings: provider chips + capability hint + Gemini plan + exec toggle + model + key)
//       | 'compact' (ANIMA drawer: provider chips + model + key). onChange(cfg) fires after load + every
//       mutation so the host can refresh dependent UI (preset engine, capability cards). exec:false hides
//       the browser/device toggle (kept from the loaded value); tier:false hides the Gemini plan row.

import * as AI from './ai.js';
import fs from './nucleo-fs.js';     // the ANIMA workspace (SOUL.md, permissions.json...): atomic, space-checked
import * as GPU from './webllm.js';   // browser-GPU models (WebGPU): offered wherever the browser has WebGPU

const STR = {
  it: {
    checking: 'controllo…', saving: 'salvo…', testing: 'provo…', deleting: 'elimino…',
    active: 'Chiave attiva:', nokey: 'Nessuna chiave per questo provider.', noset: 'Nessuna chiave impostata.',
    typefirst: 'Scrivi prima una chiave.', notlooklike: 'Non sembra una chiave ', saveanyway: '. Salvo lo stesso?',
    saved: 'Salvata:', deleted: 'Chiave eliminata.', delActive: 'Chiave eliminata. Attivo: ',
    confirmDel: 'Elimino la chiave ', confirmDelTail: ' da questo dispositivo?',
    pair: 'Accoppia il browser per gestire la chiave.', pairSave: 'Accoppia prima il browser (pairing).',
    cantread: 'Non riesco a leggere la chiave.', savefail: 'Salvataggio fallito.', delfail: 'Eliminazione fallita.',
    works: '✓ La chiave funziona — ', rejected: '✗ Chiave rifiutata.', typeortest: 'Inserisci o salva prima una chiave.',
    key: 'Chiave', model: 'Modello', exec: 'Esecuzione', plan: 'Piano', detect: '🔎 Rileva piano',
    browser: 'Browser', device: 'Device', save: '💾 Salva', test: '⚡ Prova', del: '🗑 Elimina',
    note: '🔒 La chiave è salvata solo sul tuo dispositivo (SD), mai inviata altrove né nei log. In Browser resta nel tuo browser e va diretta al provider — il dispositivo non viene caricato.',
    execBrowser: 'Le superfici web chiamano il provider direttamente dal browser: il dispositivo non viene caricato.',
    execDevice: 'Il dispositivo effettua le chiamate online (TLS leggero, gestito dall’arbitro anti-OOM).',
    paid: 'a pagamento · Pro disponibile', free: 'Free tier · solo Flash', noplan: 'piano non rilevato',
    base: 'Server', refresh: '↻ Elenco', listing: 'chiedo i modelli al server…',
    listed: (n) => n + ' modelli disponibili', nolist: 'Il server non ha restituito modelli', typemodel: 'nome del modello (es. llama3.2)',
    localok: (n, m) => '✓ Server raggiungibile — ' + n + ' modelli' + (m ? ' · prova ' + m + ' ok' : ''),
    needbase: 'Inserisci l’indirizzo del server (http://<pc>:<porta>/v1).', needmodel: 'Scegli o scrivi il modello.',
    localnote: 'Server locale: il dispositivo fa da ponte (il browser non può parlare direttamente con Ollama). La chiave serve solo se il server la richiede. Su Ollama, sul PC: OLLAMA_HOST=0.0.0.0 ollama serve.',
    netmode: 'Modalità', nets: [['offline', 'Offline', 'Solo il dispositivo, niente rete'], ['local', 'Locale', 'Dispositivo + server LLM nella tua rete, niente internet'],
           ['hybrid', 'Ibrida', 'Dispositivo, poi Wikipedia, poi il modello'], ['llm', 'LLM', 'Prima il modello, il dispositivo come riserva']],
    tg: 'Telegram: parla con ANIMA da ovunque', tgtoken: 'Token del bot', tgsave: 'Collega bot', tgon: 'Attivo', tgunlink: 'Scollega chat', tgforget: 'Rimuovi bot',
    tgsteps: '1) Su Telegram scrivi a @BotFather: /newbot, scegli un nome, copia il token qui. 2) Apri il tuo bot e invia il comando qui sotto. Solo la tua chat riceverà risposta; gli altri vengono rifiutati.',
    tgst: { none: 'nessun bot', checking: 'controllo il token…', wait: 'bot @%b pronto: invia al bot', paired: 'collegato a @%b: scrivigli quando vuoi (anche le notifiche dei controlli proattivi arrivano lì)', off: 'bot @%b in pausa' },
    ws: 'Workspace di ANIMA (stile OpenClaw)', wsfile: 'File', wssave: 'Salva', wsex: 'Esempio', wssaved: 'salvato', wsempty: '(vuoto: non usato)',
    wsdesc: { 'SOUL.md': 'Chi è ANIMA: tono, valori, limiti. Va nel prompt del modello a ogni risposta.', 'USER.md': 'Chi sei tu: nome, abitudini, preferenze. Va nel prompt del modello.', 'HEARTBEAT.md': 'La checklist dei controlli proattivi: ANIMA la rilegge ogni tanto e ti avvisa solo se serve.' },
    wsexample: {
      'SOUL.md': '# Chi sei\nSei ANIMA: calda, diretta, con un filo di ironia. Risposte brevi, a meno che non ti chieda dettagli.\nNon fingere mai di aver fatto qualcosa che non hai fatto. Chiedi prima di qualsiasi cosa irreversibile.',
      'USER.md': '# Su di me\nNome: …\nCittà: … (per il meteo)\nMi piace: …\nPer favore: chiamami per nome, usa il sistema metrico.',
      'HEARTBEAT.md': '- C\'è un impegno nelle prossime 2 ore? Ricordami cosa e quando.\n- Domani mattina è piena? Dimmelo stasera dopo le 20.\n- C\'è qualcosa che ti ho chiesto di ricordarmi oggi?' },
    perm: 'Permessi delle azioni del modello', permlv: { allow: 'consenti', ask: 'chiedi', deny: 'nega' },
    permnames: { open_app: 'aprire app', close_app: 'fermare la musica', set_volume: 'volume', set_brightness: 'luminosità', add_event: 'promemoria/calendario', create_file: 'creare file', remember: 'ricordare fatti (Memoria, nella chat web)', sh: 'comandi shell che modificano (rm, cp, mkdir, store install…)', write: 'scrivere e modificare file (app, script)', rule: 'creare automazioni (regole)' },
    permauto: 'Modalità autonoma: le azioni su "chiedi" partono senza conferma (come Claude Code con i permessi saltati). "Nega" resta negato; i comandi di sola lettura (ls, cat, df…) non chiedono mai.',
    permnote: '"chiedi": ANIMA propone l\'azione e aspetta il tuo sì. Vale per le azioni decise da un modello; i comandi che dai tu restano diretti.',
    hb: 'Controlli proattivi', hbev: [[0, 'spenti'], [15, 'ogni 15 min'], [30, 'ogni 30 min'], [60, 'ogni ora']], hbnext: (n) => n < 0 ? 'nessuna checklist (scrivi HEARTBEAT.md)' : `prossimo tra ${n} min`,
    wake: 'Voce a mani libere', wakeon: 'Ascolta la parola di attivazione', wakeword: 'Parola', wakesens: 'Sensibilità', sens: ['Bassa', 'Normale', 'Alta'],
    wakest: { off: 'spenta', listening: 'in ascolto', heard: 'sentita: ascolto la domanda', paused: 'in pausa', unavailable: 'non disponibile' },
    wakehint: (w) => `Di' «${w}», poi la domanda: ANIMA smette di ascoltare quando taci e risponde a voce. Tutto sul dispositivo, senza rete.`,
    waketrig: (n, s) => `${n} attivazioni` + (s >= 0 ? ` · ultima ${s < 60 ? s + ' s' : Math.round(s / 60) + ' min'} fa` : ''),
    wakestt: { home: 'La domanda viene trascritta dal server di casa ', cloud: 'La domanda viene trascritta nel cloud: ', none: 'Manca la trascrizione: imposta qui sotto un server Whisper di casa o una chiave Groq/OpenAI' },
    wakebuild: 'Questa build non include il riconoscimento: va attivata l\'opzione NV_WAKE_ESP_SR nel firmware (vedi components/nv_wake/Kconfig).',
    vis: 'Occhi di ANIMA (modello visivo)', visph: 'es. qwen2.5vl:7b', visok: 'salvato: questo modello descrive immagini e schermate', visoff: 'nessuno: le immagini le vede solo un modello che le supporta',
    visnote: 'Se il modello di chat non vede le immagini (su Ollama: /caps nell\'app ANIMA), questo modello dello stesso server guarda screenshot e foto e li descrive ad ANIMA.',
    stt: 'Trascrizione voce in casa (Whisper)', sttph: 'http://192.168.1.20:8080/inference', sttsave: 'Salva', sttok: 'salvato: la voce viene trascritta prima da questo server', sttoff: 'nessun server: si usa la chiave cloud (se c\'è)', sttbad: 'deve essere un indirizzo della rete di casa (192.168.x.x, 10.x, .local)',
    sttnote: 'Un PC di casa trascrive la voce in 99 lingue senza cloud. whisper.cpp: whisper-server -m ggml-small.bin --host 0.0.0.0 --port 8080 (indirizzo …/inference); oppure speaches / LocalAI (…/v1/audio/transcriptions).',
    gpu: 'Modello nel browser (WebGPU)', gpuprobe: 'controllo la GPU…', gpuload: '⬇ Scarica e carica', gpuunload: '⏏ Libera GPU',
    gpudel: '🗑 Rimuovi dalla cache', gputest: '⚡ Prova', gpuuse: 'Usa nel Copilot quando il dispositivo non sa rispondere',
    gpunone: 'WebGPU non disponibile: ', gpucached: 'in cache', gpubig: 'troppo grande per questa GPU', gpuready: '✓ Pronto sulla GPU: ',
    gpunote: 'Gira sulla GPU di questo computer: niente cloud, niente chiavi. Il primo caricamento scarica i pesi (centinaia di MB, poi restano in cache).',
    gpuhello: 'Saluta in una breve frase.',
  },
  en: {
    checking: 'checking…', saving: 'saving…', testing: 'testing…', deleting: 'deleting…',
    active: 'Active key:', nokey: 'No key for this provider.', noset: 'No key set.',
    typefirst: 'Type a key first.', notlooklike: 'That does not look like a ', saveanyway: ' key. Save anyway?',
    saved: 'Saved:', deleted: 'Key deleted.', delActive: 'Key deleted. Active: ',
    confirmDel: 'Delete the ', confirmDelTail: ' key from this device?',
    pair: 'Pair this browser to manage the key.', pairSave: 'Pair this browser first (Settings ▸ pairing).',
    cantread: 'Could not read the key.', savefail: 'Save failed.', delfail: 'Delete failed.',
    works: '✓ Key works — ', rejected: '✗ Key rejected.', typeortest: 'Type or save a key first.',
    key: 'Key', model: 'Model', exec: 'Execution', plan: 'Plan', detect: '🔎 Detect plan',
    browser: 'Browser', device: 'Device', save: '💾 Save', test: '⚡ Test', del: '🗑 Delete',
    note: '🔒 The key is stored only on your device (SD), never sent elsewhere or logged. In Browser it stays in your browser and goes straight to the provider — the device isn’t loaded.',
    execBrowser: 'Web surfaces call the provider straight from the browser: the device isn’t loaded.',
    execDevice: 'The device makes the online calls (light TLS, handled by the anti-OOM arbiter).',
    paid: 'paid · Pro available', free: 'Free tier · Flash only', noplan: 'plan not detected',
    base: 'Server', refresh: '↻ List', listing: 'asking the server for its models…',
    listed: (n) => n + ' models available', nolist: 'The server returned no models', typemodel: 'model name (e.g. llama3.2)',
    localok: (n, m) => '✓ Server reachable — ' + n + ' models' + (m ? ' · ' + m + ' answered' : ''),
    needbase: 'Enter the server address (http://<pc>:<port>/v1).', needmodel: 'Pick or type the model.',
    localnote: 'Local server: the device bridges it (a browser cannot talk to Ollama directly). A key only if the server wants one. For Ollama, on the PC: OLLAMA_HOST=0.0.0.0 ollama serve.',
    netmode: 'Mode', nets: [['offline', 'Offline', 'The device only, no network'], ['local', 'Local', 'Device + an LLM server on your network, no internet'],
           ['hybrid', 'Hybrid', 'Device, then Wikipedia, then the model'], ['llm', 'LLM', 'The model first, the device as fallback']],
    tg: 'Telegram: talk to ANIMA from anywhere', tgtoken: 'Bot token', tgsave: 'Connect bot', tgon: 'Active', tgunlink: 'Unpair chat', tgforget: 'Remove bot',
    tgsteps: '1) In Telegram message @BotFather: /newbot, pick a name, paste the token here. 2) Open your bot and send the command below. Only your chat gets answers; anyone else is refused.',
    tgst: { none: 'no bot', checking: 'checking the token…', wait: 'bot @%b ready: send the bot', paired: 'paired with @%b: message it any time (proactive-check notifications arrive there too)', off: 'bot @%b paused' },
    ws: 'ANIMA workspace (OpenClaw-style)', wsfile: 'File', wssave: 'Save', wsex: 'Example', wssaved: 'saved', wsempty: '(empty: not used)',
    wsdesc: { 'SOUL.md': 'Who ANIMA is: tone, values, limits. Goes into the model prompt on every answer.', 'USER.md': 'Who you are: name, habits, preferences. Goes into the model prompt.', 'HEARTBEAT.md': 'The proactive checklist: ANIMA re-reads it now and then and notifies you only when needed.' },
    wsexample: {
      'SOUL.md': '# Who you are\nYou are ANIMA: warm, direct, a bit witty. Short answers unless asked for detail.\nNever pretend to have done something you did not do. Ask before anything irreversible.',
      'USER.md': '# About me\nName: …\nCity: … (for weather)\nI like: …\nPlease: call me by name, use metric units.',
      'HEARTBEAT.md': '- Is there an event in the next 2 hours? Remind me what and when.\n- Is tomorrow morning busy? Tell me tonight after 20:00.\n- Anything I asked to be reminded of today?' },
    perm: 'Permissions for model actions', permlv: { allow: 'allow', ask: 'ask', deny: 'deny' },
    permnames: { open_app: 'open apps', close_app: 'stop music', set_volume: 'volume', set_brightness: 'brightness', add_event: 'reminders/calendar', create_file: 'create files', remember: 'remember facts (Memory, in the web chat)', sh: 'shell commands that change things (rm, cp, mkdir, store install…)', write: 'write and edit files (apps, scripts)', rule: 'create automations (rules)' },
    permauto: 'Autonomous mode: actions set to "ask" run without confirmation (like Claude Code skipping permissions). "Deny" still holds; read-only commands (ls, cat, df…) never ask.',
    permnote: '"ask": ANIMA proposes the action and waits for your yes. Applies to actions a model decides; your own commands stay direct.',
    hb: 'Proactive checks', hbev: [[0, 'off'], [15, 'every 15 min'], [30, 'every 30 min'], [60, 'hourly']], hbnext: (n) => n < 0 ? 'no checklist (write HEARTBEAT.md)' : `next in ${n} min`,
    wake: 'Hands-free voice', wakeon: 'Listen for the wake word', wakeword: 'Word', wakesens: 'Sensitivity', sens: ['Low', 'Normal', 'High'],
    wakest: { off: 'off', listening: 'listening', heard: 'heard: taking the question', paused: 'paused', unavailable: 'unavailable' },
    wakehint: (w) => `Say "${w}", then your question: ANIMA stops listening when you go quiet and answers aloud. All on the device, no network.`,
    waketrig: (n, s) => `${n} activations` + (s >= 0 ? ` · last ${s < 60 ? s + ' s' : Math.round(s / 60) + ' min'} ago` : ''),
    wakestt: { home: 'The question is transcribed by the home server ', cloud: 'The question is transcribed in the cloud: ', none: 'No transcription set: add a home Whisper server below or a Groq/OpenAI key' },
    wakebuild: 'This build has no detector: enable NV_WAKE_ESP_SR in the firmware (see components/nv_wake/Kconfig).',
    vis: 'ANIMA\'s eyes (vision model)', visph: 'e.g. qwen2.5vl:7b', visok: 'saved: this model describes images and screenshots', visoff: 'none: only a model that supports images can see them',
    visnote: 'If the chat model cannot see images (on Ollama: /caps in the ANIMA app), this model on the same server looks at screenshots and photos and describes them to ANIMA.',
    stt: 'Voice transcription at home (Whisper)', sttph: 'http://192.168.1.20:8080/inference', sttsave: 'Save', sttok: 'saved: voice is transcribed by this server first', sttoff: 'no server: the cloud key is used (if any)', sttbad: 'must be a home-network address (192.168.x.x, 10.x, .local)',
    sttnote: 'A home PC transcribes voice in 99 languages with no cloud. whisper.cpp: whisper-server -m ggml-small.bin --host 0.0.0.0 --port 8080 (address …/inference); or speaches / LocalAI (…/v1/audio/transcriptions).',
    gpu: 'Model in the browser (WebGPU)', gpuprobe: 'checking the GPU…', gpuload: '⬇ Download & load', gpuunload: '⏏ Free the GPU',
    gpudel: '🗑 Remove from cache', gputest: '⚡ Test', gpuuse: 'Use it in the Copilot when the device has no answer',
    gpunone: 'WebGPU not available: ', gpucached: 'cached', gpubig: 'too big for this GPU', gpuready: '✓ Ready on the GPU: ',
    gpunote: 'Runs on this computer’s GPU: no cloud, no keys. The first load downloads the weights (hundreds of MB, then cached).',
    gpuhello: 'Say hello in one short sentence.',
  },
  es: {
    checking: 'comprobando…', saving: 'guardando…', testing: 'probando…', deleting: 'eliminando…',
    active: 'Clave activa:', nokey: 'No hay clave para este proveedor.', noset: 'Ninguna clave configurada.',
    typefirst: 'Escribe primero una clave.', notlooklike: 'No parece una clave ', saveanyway: '. ¿Guardar de todos modos?',
    saved: 'Guardada:', deleted: 'Clave eliminada.', delActive: 'Clave eliminada. Activa: ',
    confirmDel: '¿Eliminar la clave ', confirmDelTail: ' de este dispositivo?',
    pair: 'Empareja este navegador para gestionar la clave.', pairSave: 'Empareja primero este navegador (Ajustes ▸ emparejamiento).',
    cantread: 'No se puede leer la clave.', savefail: 'Error al guardar.', delfail: 'Error al eliminar.',
    works: '✓ La clave funciona — ', rejected: '✗ Clave rechazada.', typeortest: 'Introduce o guarda primero una clave.',
    key: 'Clave', model: 'Modelo', exec: 'Ejecución', plan: 'Plan', detect: '🔎 Detectar plan',
    browser: 'Navegador', device: 'Dispositivo', save: '💾 Guardar', test: '⚡ Probar', del: '🗑 Eliminar',
    note: '🔒 La clave se guarda solo en tu dispositivo (SD), nunca se envía a otro sitio ni se registra. En Navegador permanece en tu navegador y va directa al proveedor — el dispositivo no se carga.',
    execBrowser: 'Las superficies web llaman al proveedor directamente desde el navegador: el dispositivo no se carga.',
    execDevice: 'El dispositivo realiza las llamadas online (TLS ligero, gestionado por el árbitro anti-OOM).',
    paid: 'de pago · Pro disponible', free: 'Nivel gratis · solo Flash', noplan: 'plan no detectado',
    base: 'Servidor', refresh: '↻ Lista', listing: 'pidiendo los modelos al servidor…',
    listed: (n) => n + ' modelos disponibles', nolist: 'El servidor no devolvió ningún modelo', typemodel: 'nombre del modelo (p. ej. llama3.2)',
    localok: (n, m) => '✓ Servidor accesible — ' + n + ' modelos' + (m ? ' · ' + m + ' respondió' : ''),
    needbase: 'Introduce la dirección del servidor (http://<pc>:<puerto>/v1).', needmodel: 'Elige o escribe el modelo.',
    localnote: 'Servidor local: el dispositivo hace de puente (un navegador no puede hablar directamente con Ollama). La clave solo si el servidor la pide. Con Ollama, en el PC: OLLAMA_HOST=0.0.0.0 ollama serve.',
    netmode: 'Modo', nets: [['offline', 'Sin red', 'Solo el dispositivo, sin red'], ['local', 'Local', 'Dispositivo + un servidor LLM en tu red, sin internet'],
           ['hybrid', 'Híbrido', 'Dispositivo, luego Wikipedia, luego el modelo'], ['llm', 'LLM', 'Primero el modelo, el dispositivo como respaldo']],
    tg: 'Telegram: habla con ANIMA desde cualquier lugar', tgtoken: 'Token del bot', tgsave: 'Conectar bot', tgon: 'Activo', tgunlink: 'Desvincular chat', tgforget: 'Quitar bot',
    tgsteps: '1) En Telegram escribe a @BotFather: /newbot, elige un nombre y pega aquí el token. 2) Abre tu bot y envíale el comando de abajo. Solo tu chat recibe respuestas; cualquier otro es rechazado.',
    tgst: { none: 'ningún bot', checking: 'compruebo el token…', wait: 'bot @%b listo: envía al bot', paired: 'vinculado a @%b: escríbele cuando quieras (los avisos de las comprobaciones proactivas también llegan ahí)', off: 'bot @%b en pausa' },
    ws: 'Espacio de trabajo de ANIMA (estilo OpenClaw)', wsfile: 'Archivo', wssave: 'Guardar', wsex: 'Ejemplo', wssaved: 'guardado', wsempty: '(vacío: no se usa)',
    wsdesc: { 'SOUL.md': 'Quién es ANIMA: tono, valores, límites. Va en el prompt del modelo en cada respuesta.', 'USER.md': 'Quién eres tú: nombre, costumbres, preferencias. Va en el prompt del modelo.', 'HEARTBEAT.md': 'La lista de comprobaciones proactivas: ANIMA la relee de vez en cuando y solo te avisa cuando hace falta.' },
    wsexample: {
      'SOUL.md': '# Quién eres\nEres ANIMA: cálida, directa, con un toque de ironía. Respuestas breves, salvo que te pida detalles.\nNunca finjas haber hecho algo que no hiciste. Pregunta antes de cualquier cosa irreversible.',
      'USER.md': '# Sobre mí\nNombre: …\nCiudad: … (para el tiempo)\nMe gusta: …\nPor favor: llámame por mi nombre, usa el sistema métrico.',
      'HEARTBEAT.md': '- ¿Hay una cita en las próximas 2 horas? Recuérdame qué y cuándo.\n- ¿Mañana por la mañana está llena? Dímelo esta noche después de las 20:00.\n- ¿Hay algo que te pedí que me recordaras hoy?' },
    perm: 'Permisos de las acciones del modelo', permlv: { allow: 'permitir', ask: 'preguntar', deny: 'denegar' },
    permnames: { open_app: 'abrir apps', close_app: 'parar la música', set_volume: 'volumen', set_brightness: 'brillo', add_event: 'recordatorios/calendario', create_file: 'crear archivos', remember: 'recordar datos (Memoria, en el chat web)', sh: 'comandos de shell que modifican (rm, cp, mkdir, store install…)', write: 'escribir y editar archivos (apps, scripts)', rule: 'crear automatizaciones (reglas)' },
    permauto: 'Modo autónomo: las acciones en "preguntar" se ejecutan sin confirmación (como Claude Code saltándose los permisos). "Denegar" sigue denegado; los comandos de solo lectura (ls, cat, df…) nunca preguntan.',
    permnote: '"preguntar": ANIMA propone la acción y espera tu sí. Vale para las acciones que decide un modelo; tus propios comandos siguen siendo directos.',
    hb: 'Comprobaciones proactivas', hbev: [[0, 'apagadas'], [15, 'cada 15 min'], [30, 'cada 30 min'], [60, 'cada hora']], hbnext: (n) => n < 0 ? 'sin lista (escribe HEARTBEAT.md)' : `próxima en ${n} min`,
    wake: 'Voz manos libres', wakeon: 'Escuchar la palabra de activación', wakeword: 'Palabra', wakesens: 'Sensibilidad', sens: ['Baja', 'Normal', 'Alta'],
    wakest: { off: 'apagada', listening: 'escuchando', heard: 'oída: escucho la pregunta', paused: 'en pausa', unavailable: 'no disponible' },
    wakehint: (w) => `Di «${w}» y luego tu pregunta: ANIMA deja de escuchar cuando callas y responde en voz alta. Todo en el dispositivo, sin red.`,
    waketrig: (n, s) => `${n} activaciones` + (s >= 0 ? ` · la última hace ${s < 60 ? s + ' s' : Math.round(s / 60) + ' min'}` : ''),
    wakestt: { home: 'La pregunta la transcribe el servidor de casa ', cloud: 'La pregunta se transcribe en la nube: ', none: 'Falta la transcripción: configura abajo un servidor Whisper de casa o una clave Groq/OpenAI' },
    wakebuild: 'Esta compilación no incluye el detector: activa la opción NV_WAKE_ESP_SR del firmware (ver components/nv_wake/Kconfig).',
    vis: 'Los ojos de ANIMA (modelo de visión)', visph: 'p. ej. qwen2.5vl:7b', visok: 'guardado: este modelo describe imágenes y capturas', visoff: 'ninguno: solo un modelo que admite imágenes puede verlas',
    visnote: 'Si el modelo de chat no ve imágenes (en Ollama: /caps en la app ANIMA), este modelo del mismo servidor mira capturas y fotos y se las describe a ANIMA.',
    stt: 'Transcripción de voz en casa (Whisper)', sttph: 'http://192.168.1.20:8080/inference', sttsave: 'Guardar', sttok: 'guardado: la voz la transcribe primero este servidor', sttoff: 'ningún servidor: se usa la clave en la nube (si la hay)', sttbad: 'debe ser una dirección de la red de casa (192.168.x.x, 10.x, .local)',
    sttnote: 'Un PC de casa transcribe la voz en 99 idiomas sin nube. whisper.cpp: whisper-server -m ggml-small.bin --host 0.0.0.0 --port 8080 (dirección …/inference); o speaches / LocalAI (…/v1/audio/transcriptions).',
    gpu: 'Modelo en el navegador (WebGPU)', gpuprobe: 'compruebo la GPU…', gpuload: '⬇ Descargar y cargar', gpuunload: '⏏ Liberar la GPU',
    gpudel: '🗑 Quitar de la caché', gputest: '⚡ Probar', gpuuse: 'Usarlo en el Copilot cuando el dispositivo no sabe responder',
    gpunone: 'WebGPU no disponible: ', gpucached: 'en caché', gpubig: 'demasiado grande para esta GPU', gpuready: '✓ Listo en la GPU: ',
    gpunote: 'Funciona en la GPU de este ordenador: sin nube, sin claves. La primera carga descarga los pesos (cientos de MB, luego quedan en caché).',
    gpuhello: 'Saluda en una frase corta.',
  },
  fr: {
    checking: 'vérification…', saving: 'enregistrement…', testing: 'test…', deleting: 'suppression…',
    active: 'Clé active :', nokey: 'Aucune clé pour ce fournisseur.', noset: 'Aucune clé définie.',
    typefirst: 'Saisissez d’abord une clé.', notlooklike: 'Cela ne ressemble pas à une clé ', saveanyway: '. Enregistrer quand même ?',
    saved: 'Enregistrée :', deleted: 'Clé supprimée.', delActive: 'Clé supprimée. Active : ',
    confirmDel: 'Supprimer la clé ', confirmDelTail: ' de cet appareil ?',
    pair: 'Appairez ce navigateur pour gérer la clé.', pairSave: 'Appairez d’abord ce navigateur (Réglages ▸ appairage).',
    cantread: 'Impossible de lire la clé.', savefail: 'Échec de l’enregistrement.', delfail: 'Échec de la suppression.',
    works: '✓ La clé fonctionne — ', rejected: '✗ Clé refusée.', typeortest: 'Saisissez ou enregistrez d’abord une clé.',
    key: 'Clé', model: 'Modèle', exec: 'Exécution', plan: 'Forfait', detect: '🔎 Détecter le forfait',
    browser: 'Navigateur', device: 'Appareil', save: '💾 Enregistrer', test: '⚡ Tester', del: '🗑 Supprimer',
    note: '🔒 La clé est enregistrée uniquement sur votre appareil (SD), jamais envoyée ailleurs ni journalisée. En mode Navigateur, elle reste dans votre navigateur et va directement au fournisseur — l’appareil n’est pas sollicité.',
    execBrowser: 'Les surfaces web appellent le fournisseur directement depuis le navigateur : l’appareil n’est pas sollicité.',
    execDevice: 'L’appareil effectue les appels en ligne (TLS léger, géré par l’arbitre anti-OOM).',
    paid: 'payant · Pro disponible', free: 'Offre gratuite · Flash uniquement', noplan: 'forfait non détecté',
    base: 'Serveur', refresh: '↻ Liste', listing: 'je demande ses modèles au serveur…',
    listed: (n) => n + ' modèles disponibles', nolist: 'Le serveur n’a renvoyé aucun modèle', typemodel: 'nom du modèle (ex. llama3.2)',
    localok: (n, m) => '✓ Serveur joignable — ' + n + ' modèles' + (m ? ' · ' + m + ' a répondu' : ''),
    needbase: 'Saisissez l’adresse du serveur (http://<pc>:<port>/v1).', needmodel: 'Choisissez ou saisissez le modèle.',
    localnote: 'Serveur local : l’appareil sert de pont (un navigateur ne peut pas parler directement à Ollama). Une clé seulement si le serveur en demande une. Pour Ollama, sur le PC : OLLAMA_HOST=0.0.0.0 ollama serve.',
    netmode: 'Mode', nets: [['offline', 'Hors ligne', 'L’appareil seul, sans réseau'], ['local', 'Local', 'Appareil + un serveur LLM sur votre réseau, sans internet'],
           ['hybrid', 'Hybride', 'L’appareil, puis Wikipédia, puis le modèle'], ['llm', 'LLM', 'Le modèle d’abord, l’appareil en secours']],
    tg: 'Telegram : parlez à ANIMA de partout', tgtoken: 'Jeton du bot', tgsave: 'Connecter le bot', tgon: 'Actif', tgunlink: 'Dissocier le chat', tgforget: 'Supprimer le bot',
    tgsteps: '1) Dans Telegram, écrivez à @BotFather : /newbot, choisissez un nom, collez le jeton ici. 2) Ouvrez votre bot et envoyez-lui la commande ci-dessous. Seul votre chat reçoit des réponses ; tous les autres sont refusés.',
    tgst: { none: 'aucun bot', checking: 'vérification du jeton…', wait: 'bot @%b prêt : envoyez au bot', paired: 'associé à @%b : écrivez-lui quand vous voulez (les notifications des vérifications proactives arrivent aussi là)', off: 'bot @%b en pause' },
    ws: 'Espace de travail d’ANIMA (style OpenClaw)', wsfile: 'Fichier', wssave: 'Enregistrer', wsex: 'Exemple', wssaved: 'enregistré', wsempty: '(vide : non utilisé)',
    wsdesc: { 'SOUL.md': 'Qui est ANIMA : ton, valeurs, limites. Va dans le prompt du modèle à chaque réponse.', 'USER.md': 'Qui vous êtes : nom, habitudes, préférences. Va dans le prompt du modèle.', 'HEARTBEAT.md': 'La liste des vérifications proactives : ANIMA la relit de temps en temps et ne vous prévient que si nécessaire.' },
    wsexample: {
      'SOUL.md': '# Qui tu es\nTu es ANIMA : chaleureuse, directe, avec une pointe d’humour. Réponses courtes, sauf si je demande des détails.\nNe prétends jamais avoir fait ce que tu n’as pas fait. Demande avant toute action irréversible.',
      'USER.md': '# À propos de moi\nNom : …\nVille : … (pour la météo)\nJ’aime : …\nS’il te plaît : appelle-moi par mon prénom, utilise le système métrique.',
      'HEARTBEAT.md': '- Y a-t-il un rendez-vous dans les 2 prochaines heures ? Rappelle-moi quoi et quand.\n- Demain matin est-il chargé ? Dis-le-moi ce soir après 20 h.\n- Y a-t-il quelque chose que je t’ai demandé de me rappeler aujourd’hui ?' },
    perm: 'Autorisations des actions du modèle', permlv: { allow: 'autoriser', ask: 'demander', deny: 'refuser' },
    permnames: { open_app: 'ouvrir des apps', close_app: 'arrêter la musique', set_volume: 'volume', set_brightness: 'luminosité', add_event: 'rappels/calendrier', create_file: 'créer des fichiers', remember: 'mémoriser des faits (Mémoire, dans le chat web)', sh: 'commandes shell qui modifient (rm, cp, mkdir, store install…)', write: 'écrire et modifier des fichiers (apps, scripts)', rule: 'créer des automatisations (règles)' },
    permauto: 'Mode autonome : les actions sur « demander » s’exécutent sans confirmation (comme Claude Code sans autorisations). « Refuser » reste refusé ; les commandes en lecture seule (ls, cat, df…) ne demandent jamais.',
    permnote: '« demander » : ANIMA propose l’action et attend votre oui. Valable pour les actions décidées par un modèle ; vos propres commandes restent directes.',
    hb: 'Vérifications proactives', hbev: [[0, 'désactivées'], [15, 'toutes les 15 min'], [30, 'toutes les 30 min'], [60, 'toutes les heures']], hbnext: (n) => n < 0 ? 'aucune liste (écrivez HEARTBEAT.md)' : `prochaine dans ${n} min`,
    wake: 'Voix mains libres', wakeon: 'Écouter le mot d’activation', wakeword: 'Mot', wakesens: 'Sensibilité', sens: ['Faible', 'Normale', 'Élevée'],
    wakest: { off: 'désactivée', listening: 'à l’écoute', heard: 'entendu : j’écoute la question', paused: 'en pause', unavailable: 'indisponible' },
    wakehint: (w) => `Dites « ${w} », puis votre question : ANIMA arrête d’écouter quand vous vous taisez et répond à voix haute. Tout sur l’appareil, sans réseau.`,
    waketrig: (n, s) => `${n} activations` + (s >= 0 ? ` · dernière il y a ${s < 60 ? s + ' s' : Math.round(s / 60) + ' min'}` : ''),
    wakestt: { home: 'La question est transcrite par le serveur de la maison ', cloud: 'La question est transcrite dans le cloud : ', none: 'Aucune transcription : configurez ci-dessous un serveur Whisper à la maison ou une clé Groq/OpenAI' },
    wakebuild: 'Cette version n’inclut pas le détecteur : activez l’option NV_WAKE_ESP_SR du firmware (voir components/nv_wake/Kconfig).',
    vis: 'Les yeux d’ANIMA (modèle de vision)', visph: 'ex. qwen2.5vl:7b', visok: 'enregistré : ce modèle décrit images et captures d’écran', visoff: 'aucun : seul un modèle qui prend en charge les images peut les voir',
    visnote: 'Si le modèle de chat ne voit pas les images (sur Ollama : /caps dans l’app ANIMA), ce modèle du même serveur regarde captures et photos et les décrit à ANIMA.',
    stt: 'Transcription vocale à la maison (Whisper)', sttph: 'http://192.168.1.20:8080/inference', sttsave: 'Enregistrer', sttok: 'enregistré : la voix est d’abord transcrite par ce serveur', sttoff: 'aucun serveur : la clé cloud est utilisée (s’il y en a une)', sttbad: 'doit être une adresse du réseau domestique (192.168.x.x, 10.x, .local)',
    sttnote: 'Un PC de la maison transcrit la voix en 99 langues sans cloud. whisper.cpp : whisper-server -m ggml-small.bin --host 0.0.0.0 --port 8080 (adresse …/inference) ; ou speaches / LocalAI (…/v1/audio/transcriptions).',
    gpu: 'Modèle dans le navigateur (WebGPU)', gpuprobe: 'vérification du GPU…', gpuload: '⬇ Télécharger et charger', gpuunload: '⏏ Libérer le GPU',
    gpudel: '🗑 Retirer du cache', gputest: '⚡ Tester', gpuuse: 'L’utiliser dans le Copilot quand l’appareil n’a pas de réponse',
    gpunone: 'WebGPU indisponible : ', gpucached: 'en cache', gpubig: 'trop grand pour ce GPU', gpuready: '✓ Prêt sur le GPU : ',
    gpunote: 'Tourne sur le GPU de cet ordinateur : pas de cloud, pas de clés. Le premier chargement télécharge les poids (des centaines de Mo, ensuite en cache).',
    gpuhello: 'Dis bonjour en une courte phrase.',
  },
  de: {
    checking: 'prüfe…', saving: 'speichere…', testing: 'teste…', deleting: 'lösche…',
    active: 'Aktiver Schlüssel:', nokey: 'Kein Schlüssel für diesen Anbieter.', noset: 'Kein Schlüssel festgelegt.',
    typefirst: 'Gib zuerst einen Schlüssel ein.', notlooklike: 'Das sieht nicht nach einem ', saveanyway: '-Schlüssel aus. Trotzdem speichern?',
    saved: 'Gespeichert:', deleted: 'Schlüssel gelöscht.', delActive: 'Schlüssel gelöscht. Aktiv: ',
    confirmDel: 'Den ', confirmDelTail: '-Schlüssel von diesem Gerät löschen?',
    pair: 'Koppele diesen Browser, um den Schlüssel zu verwalten.', pairSave: 'Koppele zuerst diesen Browser (Einstellungen ▸ Pairing).',
    cantread: 'Schlüssel kann nicht gelesen werden.', savefail: 'Speichern fehlgeschlagen.', delfail: 'Löschen fehlgeschlagen.',
    works: '✓ Schlüssel funktioniert — ', rejected: '✗ Schlüssel abgelehnt.', typeortest: 'Gib zuerst einen Schlüssel ein oder speichere ihn.',
    key: 'Schlüssel', model: 'Modell', exec: 'Ausführung', plan: 'Tarif', detect: '🔎 Tarif erkennen',
    browser: 'Browser', device: 'Gerät', save: '💾 Speichern', test: '⚡ Testen', del: '🗑 Löschen',
    note: '🔒 Der Schlüssel wird nur auf deinem Gerät (SD) gespeichert, nie anderswohin gesendet oder protokolliert. Im Browser-Modus bleibt er in deinem Browser und geht direkt an den Anbieter — das Gerät wird nicht belastet.',
    execBrowser: 'Die Web-Oberflächen rufen den Anbieter direkt aus dem Browser auf: das Gerät wird nicht belastet.',
    execDevice: 'Das Gerät führt die Online-Aufrufe aus (leichtes TLS, vom Anti-OOM-Arbiter verwaltet).',
    paid: 'kostenpflichtig · Pro verfügbar', free: 'Kostenlos · nur Flash', noplan: 'Tarif nicht erkannt',
    base: 'Server', refresh: '↻ Liste', listing: 'frage den Server nach seinen Modellen…',
    listed: (n) => n + ' Modelle verfügbar', nolist: 'Der Server hat keine Modelle geliefert', typemodel: 'Modellname (z. B. llama3.2)',
    localok: (n, m) => '✓ Server erreichbar — ' + n + ' Modelle' + (m ? ' · ' + m + ' hat geantwortet' : ''),
    needbase: 'Gib die Serveradresse ein (http://<pc>:<port>/v1).', needmodel: 'Wähle oder tippe das Modell.',
    localnote: 'Lokaler Server: das Gerät dient als Brücke (ein Browser kann nicht direkt mit Ollama sprechen). Ein Schlüssel nur, wenn der Server einen verlangt. Für Ollama auf dem PC: OLLAMA_HOST=0.0.0.0 ollama serve.',
    netmode: 'Modus', nets: [['offline', 'Offline', 'Nur das Gerät, kein Netz'], ['local', 'Lokal', 'Gerät + ein LLM-Server in deinem Netz, kein Internet'],
           ['hybrid', 'Hybrid', 'Gerät, dann Wikipedia, dann das Modell'], ['llm', 'LLM', 'Zuerst das Modell, das Gerät als Reserve']],
    tg: 'Telegram: sprich von überall mit ANIMA', tgtoken: 'Bot-Token', tgsave: 'Bot verbinden', tgon: 'Aktiv', tgunlink: 'Chat trennen', tgforget: 'Bot entfernen',
    tgsteps: '1) Schreibe in Telegram an @BotFather: /newbot, wähle einen Namen und füge das Token hier ein. 2) Öffne deinen Bot und sende ihm den Befehl unten. Nur dein Chat bekommt Antworten; alle anderen werden abgewiesen.',
    tgst: { none: 'kein Bot', checking: 'prüfe das Token…', wait: 'Bot @%b bereit: sende dem Bot', paired: 'verbunden mit @%b: schreib ihm jederzeit (auch die Meldungen der proaktiven Prüfungen kommen dort an)', off: 'Bot @%b pausiert' },
    ws: 'ANIMA-Arbeitsbereich (im Stil von OpenClaw)', wsfile: 'Datei', wssave: 'Speichern', wsex: 'Beispiel', wssaved: 'gespeichert', wsempty: '(leer: nicht verwendet)',
    wsdesc: { 'SOUL.md': 'Wer ANIMA ist: Ton, Werte, Grenzen. Geht bei jeder Antwort in den Prompt des Modells.', 'USER.md': 'Wer du bist: Name, Gewohnheiten, Vorlieben. Geht in den Prompt des Modells.', 'HEARTBEAT.md': 'Die Liste der proaktiven Prüfungen: ANIMA liest sie ab und zu und meldet sich nur, wenn nötig.' },
    wsexample: {
      'SOUL.md': '# Wer du bist\nDu bist ANIMA: herzlich, direkt, mit einem Hauch Ironie. Kurze Antworten, außer ich frage nach Details.\nTu nie so, als hättest du etwas getan, das du nicht getan hast. Frag vor allem, was sich nicht rückgängig machen lässt.',
      'USER.md': '# Über mich\nName: …\nStadt: … (für das Wetter)\nIch mag: …\nBitte: nenn mich beim Namen, nutze metrische Einheiten.',
      'HEARTBEAT.md': '- Gibt es einen Termin in den nächsten 2 Stunden? Erinnere mich, was und wann.\n- Ist morgen früh voll? Sag es mir heute Abend nach 20 Uhr.\n- Gibt es etwas, woran du mich heute erinnern solltest?' },
    perm: 'Berechtigungen für Aktionen des Modells', permlv: { allow: 'erlauben', ask: 'fragen', deny: 'verweigern' },
    permnames: { open_app: 'Apps öffnen', close_app: 'Musik stoppen', set_volume: 'Lautstärke', set_brightness: 'Helligkeit', add_event: 'Erinnerungen/Kalender', create_file: 'Dateien anlegen', remember: 'Fakten merken (Gedächtnis, im Web-Chat)', sh: 'Shell-Befehle, die etwas ändern (rm, cp, mkdir, store install…)', write: 'Dateien schreiben und bearbeiten (Apps, Skripte)', rule: 'Automatisierungen anlegen (Regeln)' },
    permauto: 'Autonomer Modus: Aktionen auf „fragen“ laufen ohne Bestätigung (wie Claude Code mit übersprungenen Berechtigungen). „Verweigern“ bleibt verweigert; reine Lesebefehle (ls, cat, df…) fragen nie.',
    permnote: '„fragen“: ANIMA schlägt die Aktion vor und wartet auf dein Ja. Gilt für Aktionen, die ein Modell entscheidet; deine eigenen Befehle bleiben direkt.',
    hb: 'Proaktive Prüfungen', hbev: [[0, 'aus'], [15, 'alle 15 Min.'], [30, 'alle 30 Min.'], [60, 'stündlich']], hbnext: (n) => n < 0 ? 'keine Liste (schreibe HEARTBEAT.md)' : `nächste in ${n} Min.`,
    wake: 'Freihändige Stimme', wakeon: 'Auf das Aktivierungswort hören', wakeword: 'Wort', wakesens: 'Empfindlichkeit', sens: ['Niedrig', 'Normal', 'Hoch'],
    wakest: { off: 'aus', listening: 'hört zu', heard: 'gehört: nehme die Frage auf', paused: 'pausiert', unavailable: 'nicht verfügbar' },
    wakehint: (w) => `Sag „${w}“, dann deine Frage: ANIMA hört auf zuzuhören, wenn du schweigst, und antwortet laut. Alles auf dem Gerät, ohne Netz.`,
    waketrig: (n, s) => `${n} Aktivierungen` + (s >= 0 ? ` · zuletzt vor ${s < 60 ? s + ' s' : Math.round(s / 60) + ' Min.'}` : ''),
    wakestt: { home: 'Die Frage wird vom Server zu Hause transkribiert ', cloud: 'Die Frage wird in der Cloud transkribiert: ', none: 'Keine Transkription eingestellt: richte unten einen Whisper-Server zu Hause oder einen Groq/OpenAI-Schlüssel ein' },
    wakebuild: 'Diese Version enthält keinen Detektor: aktiviere die Option NV_WAKE_ESP_SR in der Firmware (siehe components/nv_wake/Kconfig).',
    vis: 'ANIMAs Augen (Bildmodell)', visph: 'z. B. qwen2.5vl:7b', visok: 'gespeichert: dieses Modell beschreibt Bilder und Bildschirmfotos', visoff: 'keines: nur ein Modell, das Bilder unterstützt, kann sie sehen',
    visnote: 'Wenn das Chat-Modell keine Bilder sieht (bei Ollama: /caps in der ANIMA-App), schaut sich dieses Modell auf demselben Server Bildschirmfotos und Fotos an und beschreibt sie ANIMA.',
    stt: 'Sprachtranskription zu Hause (Whisper)', sttph: 'http://192.168.1.20:8080/inference', sttsave: 'Speichern', sttok: 'gespeichert: die Stimme wird zuerst von diesem Server transkribiert', sttoff: 'kein Server: der Cloud-Schlüssel wird verwendet (falls vorhanden)', sttbad: 'muss eine Adresse im Heimnetz sein (192.168.x.x, 10.x, .local)',
    sttnote: 'Ein PC zu Hause transkribiert Sprache in 99 Sprachen ohne Cloud. whisper.cpp: whisper-server -m ggml-small.bin --host 0.0.0.0 --port 8080 (Adresse …/inference); oder speaches / LocalAI (…/v1/audio/transcriptions).',
    gpu: 'Modell im Browser (WebGPU)', gpuprobe: 'prüfe die GPU…', gpuload: '⬇ Herunterladen und laden', gpuunload: '⏏ GPU freigeben',
    gpudel: '🗑 Aus dem Cache entfernen', gputest: '⚡ Testen', gpuuse: 'Im Copilot nutzen, wenn das Gerät keine Antwort hat',
    gpunone: 'WebGPU nicht verfügbar: ', gpucached: 'im Cache', gpubig: 'zu groß für diese GPU', gpuready: '✓ Bereit auf der GPU: ',
    gpunote: 'Läuft auf der GPU dieses Computers: keine Cloud, keine Schlüssel. Das erste Laden lädt die Gewichte herunter (Hunderte MB, danach im Cache).',
    gpuhello: 'Sag in einem kurzen Satz Hallo.',
  },
};

// One short capability hint per provider (full variant), derived from CAPMATRIX so it never drifts.
function capHint(p, lang) {
  const c = AI.CAPMATRIX[p] || {};
  const LBL = {
    it: { can: 'Sa: ',    no: ' · No: ',   image: 'immagini',  whisper: 'trascrizione voce',   tools: 'strumenti OS' },
    en: { can: 'Can: ',   no: ' · No: ',   image: 'image gen', whisper: 'voice transcription', tools: 'OS tools' },
    es: { can: 'Puede: ', no: ' · No: ',   image: 'imágenes',  whisper: 'transcripción de voz', tools: 'herramientas del SO' },
    fr: { can: 'Peut : ', no: ' · Non : ', image: 'images',    whisper: 'transcription vocale', tools: 'outils OS' },
    de: { can: 'Kann: ',  no: ' · Nein: ', image: 'Bilder',    whisper: 'Sprachtranskription', tools: 'OS-Tools' },
  };
  const s = LBL[lang] || LBL.it;
  const can = [], cant = [];
  // Transcription is listed as available when the provider can reach it EITHER way — a dedicated
  // Whisper endpoint or an audio-native model — because routeFor() will happily use either.
  const c2 = Object.assign({}, c, { whisper: !!(c.whisper || c.audioLLM) });
  [['chat', 'chat'], ['image', s.image], ['whisper', s.whisper], ['toolUse', s.tools]]
    .forEach(([k, lbl]) => (c2[k] ? can : cant).push(lbl));
  return s.can + can.join(', ') + (cant.length ? s.no + cant.join(', ') : '');
}

let _cssInjected = false;
function injectCss() {
  if (_cssInjected) return;
  _cssInjected = true;
  const s = document.createElement('style');
  s.textContent = `
.nkm{display:flex;flex-direction:column;gap:10px;font:inherit}
.nkm-chips{display:flex;flex-wrap:wrap;gap:6px}
.nkm-chip{cursor:pointer;border:1px solid var(--line,#2a2a35);background:var(--panel,#1a1a22);color:var(--ink,var(--muted,#cfcfe0));
  border-radius:var(--r-pill,999px);padding:5px 11px;font-size:13px;line-height:1;transition:.12s}
.nkm-chip:hover{border-color:var(--accent,#9b8cff)}
.nkm-chip.cur{background:var(--accent,#9b8cff);color:var(--accent-on,var(--ink,#0e0e12));border-color:var(--accent,#9b8cff);font-weight:600}
.nkm-chip .ok{opacity:.7;margin-left:5px}
.nkm-row{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
.nkm-row label{min-width:64px;color:var(--dim,#8a8a9a);font-size:13px}
.nkm select,.nkm input{background:var(--field,var(--bg,#0e0e12));color:var(--ink,#e8e8ee);border:1px solid var(--line,#2a2a35);
  border-radius:var(--r-sm,8px);padding:7px 9px;font:inherit;font-size:14px}
.nkm input{flex:1;min-width:180px}
.nkm select{cursor:pointer;min-width:190px}
.nkm-seg{display:inline-flex;border:1px solid var(--line,#2a2a35);border-radius:var(--r-sm,8px);overflow:hidden}
.nkm-seg .it{cursor:pointer;padding:6px 12px;font-size:13px;color:var(--dim,#8a8a9a)}
.nkm-seg .it.on{background:var(--accent,#9b8cff);color:var(--accent-on,#0e0e12);font-weight:600}
.nkm-btns{display:flex;gap:7px;flex-wrap:wrap}
.nkm-btn{cursor:pointer;border:1px solid var(--line,#2a2a35);background:var(--panel,#1a1a22);color:var(--ink,#e8e8ee);
  border-radius:var(--r-sm,8px);padding:7px 12px;font:inherit;font-size:13px}
.nkm-btn:hover{border-color:var(--accent,#9b8cff)}
.nkm-btn.primary{background:var(--accent,#9b8cff);color:var(--accent-on,#0e0e12);border-color:var(--accent,#9b8cff);font-weight:600}
.nkm-btn.danger:hover{border-color:var(--bad,var(--danger-text,#ff6b6b));color:var(--bad,var(--danger-text,#ff6b6b))}
.nkm-cap,.nkm-stat,.nkm-exechint{font-size:12.5px;color:var(--dim,#8a8a9a);line-height:1.4}
.nkm-stat b{color:var(--ink,#e8e8ee)}
.nkm-note{font-size:12px;color:var(--dim,#8a8a9a);line-height:1.45;opacity:.85}
.nkm-badge{font-size:12px;padding:2px 8px;border-radius:var(--r-pill,999px);border:1px solid var(--line,#2a2a35);color:var(--dim,#8a8a9a)}
.nkm-grp{display:flex;flex-direction:column;gap:10px}
.nkm-sec{border-top:1px solid var(--line,#2a2a35);padding-top:10px;margin-top:4px;display:flex;flex-direction:column;gap:8px}
.nkm-sec h4{margin:0;font-size:13.5px;color:var(--ink,#e8e8ee)}
.nkm-bar{height:6px;border-radius:3px;background:var(--line,#2a2a35);overflow:hidden}
.nkm-bar i{display:block;height:100%;width:0;background:var(--accent,#9b8cff);transition:width .2s}
.nkm-out{font-size:12.5px;white-space:pre-wrap;color:var(--ink,#e8e8ee);background:var(--field,#0e0e12);border:1px solid var(--line,#2a2a35);border-radius:var(--r-sm,8px);padding:7px 9px;max-height:140px;overflow:auto}
.nkm-check{display:flex;align-items:center;gap:8px;font-size:13px;color:var(--ink,#e8e8ee);cursor:pointer}
.nkm-ta{width:100%;box-sizing:border-box;min-height:120px;resize:vertical;font:13px/1.45 ui-monospace,Menlo,Consolas,monospace;background:var(--field,var(--bg,#0e0e12));color:var(--ink,#e8e8ee);border:1px solid var(--line,#2a2a35);border-radius:var(--r-sm,8px);padding:8px}
.nkm-perms{display:grid;max-width:440px;grid-template-columns:minmax(120px,1fr) auto;gap:6px 12px;align-items:center;font-size:13px}
.nkm-perms select{min-width:110px}
.nkm-pair{display:flex;align-items:center;gap:8px;margin:6px 0}
.nkm-pair code{font:600 18px/1.2 ui-monospace,Menlo,Consolas,monospace;padding:6px 12px;border-radius:var(--r-sm,8px);background:var(--field,var(--bg,#0e0e12));border:1px solid var(--accent,#9b8cff);color:var(--ink,#e8e8ee);user-select:all}
.nkm-check input{flex:0 0 auto;min-width:0;width:16px;height:16px;margin:0;padding:0}`;
  document.head.appendChild(s);
}

const esc = (s) => String(s == null ? '' : s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

export function mountKeyManager(container, opts = {}) {
  injectCss();
  let lang = ['it', 'es', 'fr', 'de'].includes(opts.lang) ? opts.lang : 'en';
  const variant = opts.variant === 'compact' ? 'compact' : 'full';
  const full = variant === 'full';
  const showExec = full && opts.exec !== false;
  const showTier = opts.tier !== false;
  const onChange = typeof opts.onChange === 'function' ? opts.onChange : () => {};
  const t = () => STR[lang] || STR.en;   // English is the fallback floor

  // The live config (one per mount). keys{} remembers every provider's saved config so a switch is lossless.
  let cfg = { provider: 'anthropic', base: AI.PROVIDERS.anthropic.base, model: AI.PROVIDERS.anthropic.def,
              key: '', version: '2023-06-01', exec: 'browser', keys: {}, geminiTier: '', geminiModels: null };
  let hydrated = false;
  const prov = () => AI.providerOf(cfg.provider);

  // ---- DOM skeleton ----
  const root = document.createElement('div');
  root.className = 'nkm';
  const providerIds = Object.keys(AI.PROVIDERS);
  root.innerHTML =
    (full ? `<div class="nkm-grp" data-el="netgrp" style="display:none"><div class="nkm-row"><label>${esc(t().netmode)}</label><span class="nkm-seg" data-el="net">${t().nets.map(([id, lbl]) => `<span class="it" data-n="${id}">${esc(lbl)}</span>`).join('')}</span></div><div class="nkm-exechint" data-el="nethint"></div></div>` : '') +
    `<div class="nkm-chips" data-el="chips"></div>` +
    (full ? `<div class="nkm-cap" data-el="cap"></div>` : '') +
    (full && showTier ? `<div class="nkm-row" data-el="tierrow" style="display:none"><label>${esc(t().plan)}</label><span class="nkm-badge" data-el="tier">…</span><button type="button" class="nkm-btn" data-el="detect">${esc(t().detect)}</button></div>` : '') +
    `<div class="nkm-row" data-el="baserow" style="display:none"><label>${esc(t().base)}</label><input data-el="base" type="url" autocomplete="off" spellcheck="false" placeholder="http://192.168.1.10:11434/v1"><span class="nkm-btns" data-el="presets"></span></div>` +
    `<div class="nkm-row"><label>${esc(t().model)}</label><select data-el="model"></select><input data-el="modeltxt" style="display:none" autocomplete="off" spellcheck="false" placeholder="${esc(t().typemodel)}"><button type="button" class="nkm-btn" data-el="refresh">${esc(t().refresh)}</button></div>` +
    `<div class="nkm-note" data-el="localnote" style="display:none">${esc(t().localnote)}</div>` +
    (showExec ? `<div class="nkm-row"><label>${esc(t().exec)}</label><span class="nkm-seg" data-el="exec"><span class="it" data-x="browser">${esc(t().browser)}</span><span class="it" data-x="device">${esc(t().device)}</span></span></div>` : '') +
    (showExec ? `<div class="nkm-exechint" data-el="exechint"></div>` : '') +
    `<div class="nkm-row"><label>${esc(t().key)}</label><input data-el="key" type="password" autocomplete="off" autocapitalize="off" spellcheck="false" placeholder="sk-ant-…"></div>` +
    `<div class="nkm-btns"><button type="button" class="nkm-btn primary" data-el="save">${esc(t().save)}</button><button type="button" class="nkm-btn" data-el="test">${esc(t().test)}</button><button type="button" class="nkm-btn danger" data-el="del">${esc(t().del)}</button></div>` +
    `<div class="nkm-stat" data-el="stat">…</div>` +
    (full ? `<div class="nkm-note">${esc(t().note)}</div>` : '') +
    // Sections the DEVICE provides (ESP32-P4 firmware today): rendered hidden, shown by deviceSections() only after
    // the device proved it serves them (sealed vault + the section's own endpoint answering). The browser-GPU section
    // is the browser's, not the device's: shown by gpuInit() wherever WebGPU exists.
    (full ? `<div class="nkm-sec" data-el="tg" style="display:none"><h4>${esc(t().tg)}</h4>` +
      `<div class="nkm-stat" data-el="tgstat">…</div>` +
      `<div class="nkm-pair" data-el="tgpair" style="display:none"><code data-el="tgcode"></code><button type="button" class="nkm-btn" data-el="tgcopy">⧉</button></div>` +
      `<div class="nkm-row"><label>${esc(t().tgtoken)}</label><input data-el="tgtoken" type="password" autocomplete="off" spellcheck="false" placeholder="123456789:AA…"><button type="button" class="nkm-btn primary" data-el="tgsave">${esc(t().tgsave)}</button></div>` +
      `<div class="nkm-btns" data-el="tgbtns" style="display:none"><label class="nkm-check"><input type="checkbox" data-el="tgon"> ${esc(t().tgon)}</label><button type="button" class="nkm-btn" data-el="tgunlink">${esc(t().tgunlink)}</button><button type="button" class="nkm-btn danger" data-el="tgforget">${esc(t().tgforget)}</button></div>` +
      `<div class="nkm-note">${esc(t().tgsteps)}</div></div>` : '') +
    (full ? `<div class="nkm-sec" data-el="ws" style="display:none"><h4>${esc(t().ws)}</h4>` +
      `<div class="nkm-row"><label>${esc(t().wsfile)}</label><span class="nkm-seg" data-el="wsfiles">${['SOUL.md', 'USER.md', 'HEARTBEAT.md'].map((f) => `<span class="it" data-f="${f}">${f}</span>`).join('')}</span></div>` +
      `<div class="nkm-note" data-el="wsdesc"></div>` +
      `<textarea data-el="wstext" rows="7" spellcheck="false" class="nkm-ta"></textarea>` +
      `<div class="nkm-btns"><button type="button" class="nkm-btn primary" data-el="wssave">${esc(t().wssave)}</button><button type="button" class="nkm-btn" data-el="wsex">${esc(t().wsex)}</button><span class="nkm-stat" data-el="wsstat"></span></div>` +
      `<div class="nkm-row"><label>${esc(t().hb)}</label><select data-el="hbevery">${t().hbev.map(([v, l]) => `<option value="${v}">${esc(l)}</option>`).join('')}</select><span class="nkm-stat" data-el="hbnext"></span></div>` +
      `<h4>${esc(t().perm)}</h4><label class="nkm-check"><input type="checkbox" data-el="permauto"> ${esc(t().permauto)}</label><div class="nkm-perms" data-el="perms"></div><div class="nkm-note">${esc(t().permnote)}</div></div>` : '') +
    (full ? `<div class="nkm-sec" data-el="wake" style="display:none"><h4>${esc(t().wake)}</h4>` +
      `<div class="nkm-stat" data-el="wakestat">…</div>` +
      `<label class="nkm-check"><input type="checkbox" data-el="wakeon"> ${esc(t().wakeon)}</label>` +
      `<div class="nkm-row" data-el="wakewordrow" style="display:none"><label>${esc(t().wakeword)}</label><select data-el="wakeword"></select></div>` +
      `<div class="nkm-row" data-el="wakesensrow" style="display:none"><label>${esc(t().wakesens)}</label><span class="nkm-seg" data-el="wakesens">${t().sens.map((l, i) => `<span class="it" data-s="${i}">${esc(l)}</span>`).join('')}</span></div>` +
      `<div class="nkm-note" data-el="wakenote"></div></div>` : '') +
    (full ? `<div class="nkm-sec" data-el="sttsec" style="display:none"><h4>${esc(t().stt)}</h4><div class="nkm-row"><input data-el="stt" type="url" autocomplete="off" spellcheck="false" placeholder="${esc(t().sttph)}"><button type="button" class="nkm-btn" data-el="sttsave">${esc(t().sttsave)}</button></div>` +
      `<div class="nkm-stat" data-el="sttstat"></div><div class="nkm-note">${esc(t().sttnote)}</div></div>` : '') +
    (full ? `<div class="nkm-sec" data-el="vissec" style="display:none"><h4>${esc(t().vis)}</h4><div class="nkm-row"><input data-el="vismodel" type="text" autocomplete="off" spellcheck="false" placeholder="${esc(t().visph)}"><button type="button" class="nkm-btn" data-el="vissave">${esc(t().sttsave)}</button></div>` +
      `<div class="nkm-stat" data-el="visstat"></div><div class="nkm-note">${esc(t().visnote)}</div></div>` : '') +
    (full ? `<div class="nkm-sec" data-el="gpu" style="display:none"><h4>${esc(t().gpu)}</h4>` +
      `<div class="nkm-stat" data-el="gpustat">${esc(t().gpuprobe)}</div>` +
      `<div class="nkm-row" data-el="gpurow" style="display:none"><label>${esc(t().model)}</label><select data-el="gpumodel"></select></div>` +
      `<div class="nkm-bar" data-el="gpubar" style="display:none"><i></i></div>` +
      `<div class="nkm-btns" data-el="gpubtns" style="display:none"><button type="button" class="nkm-btn primary" data-el="gpuload">${esc(t().gpuload)}</button><button type="button" class="nkm-btn" data-el="gputest">${esc(t().gputest)}</button><button type="button" class="nkm-btn" data-el="gpuunload">${esc(t().gpuunload)}</button><button type="button" class="nkm-btn danger" data-el="gpudel">${esc(t().gpudel)}</button></div>` +
      `<label class="nkm-check" data-el="gpuuserow" style="display:none"><input type="checkbox" data-el="gpuuse"> ${esc(t().gpuuse)}</label>` +
      `<div class="nkm-out" data-el="gpuout" style="display:none"></div>` +
      `<div class="nkm-note">${esc(t().gpunote)}</div></div>` : '');
  container.innerHTML = '';
  container.appendChild(root);
  const $ = (n) => root.querySelector(`[data-el="${n}"]`);

  const setStat = (html) => { $('stat').innerHTML = html; };
  const statText = () => (prov().local && cfg.base && cfg.model)
    ? `${t().active} <b>${esc(cfg.base)}</b> · ${esc(prov().label)} · ${esc(cfg.model)}`
    : cfg.key
    ? `${t().active} <b>${esc(AI.maskKey(cfg.key, cfg.keyHint))}</b> · ${esc(prov().label)} · ${esc(cfg.model)}`
    : t().nokey;

  function fillChips() {
    // The LAN-server provider (non-enumerable in AI.PROVIDERS) only where the device relay can reach plain-http LAN
    // hosts (AI.lanRelay()); a vault already set to it keeps its chip so it can be seen and changed.
    const ids = (AI.lanRelay() || cfg.provider === 'local') ? providerIds.concat(['local']) : providerIds;
    $('chips').innerHTML = ids.map((id) => {
      const set = !!(cfg.keys && cfg.keys[id] && cfg.keys[id].key) || (id === cfg.provider && cfg.key);
      return `<button type="button" class="nkm-chip${id === cfg.provider ? ' cur' : ''}" data-p="${id}">${esc(AI.PROVIDERS[id].label)}${set ? '<span class="ok">✓</span>' : ''}</button>`;
    }).join('');
    $('chips').querySelectorAll('[data-p]').forEach((b) => b.addEventListener('click', () => chooseProvider(b.dataset.p)));
  }
  function fillModels() {
    const sel = $('model'); if (!sel) return;
    const live = cfg.liveModels && cfg.liveModels.provider === cfg.provider ? cfg.liveModels.list : null;
    const list = live && live.length ? live
      : (cfg.provider === 'google' && cfg.geminiModels && cfg.geminiModels.length) ? cfg.geminiModels : prov().models.slice();
    if (cfg.model && !list.some((m) => m[0] === cfg.model)) list.unshift([cfg.model, cfg.model]);   // keep the saved one visible
    // A local server with no list yet: a free-text model name instead of an empty dropdown.
    const typed = !list.length;
    if ($('modeltxt')) { $('modeltxt').style.display = typed ? '' : 'none'; if (typed) $('modeltxt').value = cfg.model || ''; }
    sel.style.display = typed ? 'none' : '';
    sel.innerHTML = '';
    for (const m of list) { const o = document.createElement('option'); o.value = m[0]; o.textContent = AI.modelLabel(m); sel.appendChild(o); }
    if (typed) return;                               // the typed name stays the model
    sel.value = (cfg.model && list.some((m) => m[0] === cfg.model)) ? cfg.model : (list[0] ? list[0][0] : prov().def);
    cfg.model = sel.value;
  }
  function paint() {
    const p = prov();
    fillChips();
    if ($('key')) $('key').placeholder = p.ph;
    if (full && $('cap')) $('cap').textContent = capHint(cfg.provider, lang);
    if (full && showTier && $('tierrow')) {
      $('tierrow').style.display = cfg.provider === 'google' ? '' : 'none';
      if ($('tier')) $('tier').textContent = AI.geminiTierLabel(cfg.geminiTier, lang);
    }
    if (showExec && $('exec')) {
      // a LAN server is always reached through the device relay: "browser / device" means nothing there
      $('exec').closest('.nkm-row').style.display = prov().local ? 'none' : '';
      if ($('exechint')) $('exechint').style.display = prov().local ? 'none' : '';
      $('exec').querySelectorAll('.it').forEach((b) => b.classList.toggle('on', b.dataset.x === (cfg.exec || 'browser')));
      if ($('exechint')) $('exechint').textContent = (cfg.exec === 'device') ? t().execDevice : t().execBrowser;
    }
    const local = !!p.local;
    if ($('baserow')) { $('baserow').style.display = local ? '' : 'none'; if (local && document.activeElement !== $('base')) $('base').value = cfg.base || ''; }
    if ($('localnote')) $('localnote').style.display = local ? '' : 'none';
    if ($('presets') && local && !$('presets').childElementCount) {
      $('presets').innerHTML = (p.presets || []).map(([n, port]) => `<button type="button" class="nkm-btn" data-port="${port}">${esc(n)}</button>`).join('');
      $('presets').querySelectorAll('[data-port]').forEach((b) => b.addEventListener('click', () => {
        const host = (/^https?:\/\/([^/:]+)/.exec($('base').value || cfg.base || '') || [])[1] || '192.168.1.10';
        $('base').value = 'http://' + host + ':' + b.dataset.port + '/v1'; cfg.base = $('base').value;
      }));
    }
    fillModels();
  }

  function adopt(p) {
    const d = AI.providerOf(p), st = (cfg.keys && cfg.keys[p]) || {};
    cfg.provider = p; cfg.base = st.base || d.base; cfg.model = st.model || d.def;
    cfg.version = st.version || d.version; cfg.key = st.key || ''; cfg.keyHint = st.keyHint || ''; cfg.geminiModels = null;
  }
  function chooseProvider(p) {
    if (!AI.PROVIDERS[p]) return;
    adopt(p); paint(); setStat(statText()); if ($('key')) $('key').value = '';
    onChange(getCfg());
  }

  async function reload() {
    setStat(t().checking); if ($('key')) $('key').value = '';
    const c = await AI.readTeacher({ fresh: true });
    if (c && c.unpaired) { setStat(t().pair); return; }
    if (!c) { setStat(t().cantread); return; }
    hydrated = true;
    cfg.keys = (c.keys && typeof c.keys === 'object') ? c.keys : {};
    cfg.exec = c.exec || 'browser'; cfg.geminiTier = c.geminiTier || ''; cfg.geminiModels = null;
    cfg.provider = c.provider || 'anthropic';
    const p = prov();
    cfg.base = c.base || p.base; cfg.model = c.model || p.def; cfg.key = c.key || ''; cfg.keyHint = c.keyHint || ''; cfg.version = c.version || p.version;
    cfg.stt_url = c.stt_url || ''; cfg.stt_model = c.stt_model || '';
    cfg.vision_model = c.vision_model || ''; cfg.vision_base = c.vision_base || ''; cfg.vision_key = c.vision_key || '';
    cfg.vision = typeof c.vision === 'boolean' ? c.vision : null;
    if ($('stt')) { $('stt').value = cfg.stt_url; $('sttstat').textContent = cfg.stt_url ? t().sttok : t().sttoff; }
    if ($('vismodel')) { $('vismodel').value = cfg.vision_model; $('visstat').textContent = cfg.vision_model ? t().visok : t().visoff; }
    if (cfg.key) cfg.keys[cfg.provider] = Object.assign({ base: cfg.base, model: cfg.model, key: cfg.key, keyHint: cfg.keyHint }, cfg.provider === 'anthropic' ? { version: cfg.version } : {});
    paint(); setStat((cfg.key || (prov().local && cfg.base)) ? statText() : t().noset);
    onChange(getCfg());
  }

  async function calibrate() {
    if (cfg.provider !== 'google' || !cfg.key) return;
    try {
      const c = await AI.calibrateGemini({ base: cfg.base, key: cfg.key });
      cfg.geminiTier = c.tier; cfg.geminiModels = (c.models || []).map((id) => [id, id]);
      if (!c.models.includes(cfg.model)) cfg.model = c.recommended;
      paint();
      const lbl = c.tier === 'paid' ? t().paid : c.tier === 'free' ? t().free : t().noplan;
      setStat(statText() + ' · <b>' + esc(lbl) + '</b>');
      await AI.writeTeacher(cfg);   // persist the detected plan + recommended model
      onChange(getCfg());
    } catch {}
  }

  const pickedModel = () => (($('modeltxt') && $('modeltxt').style.display !== 'none') ? $('modeltxt').value.trim() : ($('model') ? $('model').value : '')) || cfg.model;
  async function save() {
    const v = $('key').value.trim(), p = prov();
    if (p.local) {
      cfg.base = ($('base').value || '').trim().replace(/\/+$/, '');
      if (!AI.isLanUrl(cfg.base)) { setStat(t().needbase); return; }
      cfg.model = pickedModel();
      if (!cfg.model) { setStat(t().needmodel); return; }
      if (v) cfg.key = v;
    } else {
      if (!v) { setStat(t().typefirst); return; }
      if (!p.prefix.test(v) && !confirm(t().notlooklike + p.label + t().saveanyway)) return;
      cfg.key = v; cfg.model = pickedModel();
    }
    setStat(t().saving);
    const r = await AI.writeTeacher(cfg);
    if (r === true) {
      // The key now lives on the device: read its view back so the typed key leaves the page.
      $('key').value = '';
      await reload();
      setStat(`${t().saved} <b>${esc(cfg.key ? AI.maskKey(cfg.key, cfg.keyHint) : cfg.base)}</b> · ${esc(p.label)} · ${esc(cfg.model)}`);
      fillChips();
      if (cfg.provider === 'google') calibrate();
    } else if (r === 'unpaired') setStat(t().pairSave);
    else setStat(t().savefail);
  }

  async function refreshModels() {
    const p = prov();
    const c = { provider: cfg.provider, base: p.local ? ($('base').value || '').trim() : (cfg.base || p.base), key: $('key').value.trim() || cfg.key, version: cfg.version || p.version };
    if (p.local && !AI.isLanUrl(c.base)) { setStat(t().needbase); return null; }
    if (!p.local && !c.key) { setStat(t().typeortest); return null; }
    setStat(t().listing);
    // ai.js listModels: the served ids (fresh, so a just-started server is asked again), or null when it can't be asked.
    const ids = await AI.listModels(c, { fresh: true });
    if (!ids) { setStat(t().nolist); return null; }
    const list = [...new Set(ids)].sort().map((id) => [id, id]);
    cfg.liveModels = { provider: cfg.provider, list };
    fillModels();
    setStat(list.length ? t().listed(list.length) : t().nolist);
    return list;
  }

  async function test() {
    if (prov().local) {
      const list = await refreshModels();
      if (!list) return;
      const model = pickedModel();
      if (!model) { setStat(t().listed(list.length)); return; }
      try {
        const ok = await AI.cloudPing({ provider: 'local', base: ($('base').value || '').trim(), model, key: $('key').value.trim() || cfg.key });
        setStat(t().localok(list.length, ok ? model : ''));
      } catch (e) { setStat('✗ ' + esc(String(e.message || e))); }
      return;
    }
    const v = ($('key').value.trim()) || cfg.key;
    if (!v) { setStat(t().typeortest); return; }
    const model = pickedModel();
    setStat(t().testing);
    try {
      const ok = await AI.cloudPing({ provider: cfg.provider, base: cfg.base || prov().base, model, key: v, version: cfg.version || prov().version });
      setStat(ok ? (t().works + esc(prov().label) + ' · ' + esc(model)) : t().rejected);
    } catch (e) { setStat('✗ ' + esc(String(e.message || e))); }
  }

  async function del() {
    if (!confirm(t().confirmDel + prov().label + t().confirmDelTail)) return;
    setStat(t().deleting);
    const keys = Object.assign({}, cfg.keys || {}); delete keys[cfg.provider];
    const remaining = Object.keys(keys).filter((k) => keys[k] && keys[k].key);
    try {
      if (remaining.length) {
        const np = remaining[0], e = keys[np];
        cfg.keys = keys;
        cfg.provider = np; cfg.base = e.base; cfg.model = e.model; cfg.key = e.key; cfg.keyHint = e.keyHint || ''; cfg.version = e.version || '2023-06-01';
        const r = await AI.writeTeacher(cfg);
        if (r === true) { paint(); if ($('key')) $('key').value = ''; setStat(t().delActive + esc(AI.PROVIDERS[np].label)); onChange(getCfg()); return; }
        if (r === 'unpaired') { setStat(t().pairSave); return; }
        setStat(t().delfail); return;
      }
      const r = await fetch('/api/fs/delete?path=' + encodeURIComponent(AI.AI_PATH), { method: 'POST' });   // nucleo-fs: allow (the key vault keeps its own rules)
      AI.invalidateTeacher();
      if (r.ok || r.status === 404) { cfg.key = ''; cfg.keys = {}; if ($('key')) $('key').value = ''; paint(); setStat(t().deleted); onChange(getCfg()); return; }
      if (r.status === 401 || r.status === 403) { setStat(t().pairSave); return; }
      setStat(t().delfail);
    } catch { setStat(t().delfail); }
  }

  function getCfg() { return { provider: cfg.provider, base: cfg.base, model: cfg.model, key: cfg.key, version: cfg.version, exec: cfg.exec, geminiTier: cfg.geminiTier, hasKey: !!cfg.key, keys: cfg.keys }; }

  // ======== sections beyond the key: each appears ONLY where it is proven to work ========
  // Device sections (network mode, Telegram, workspace/heartbeat/permissions, wake word, home Whisper, vision helper)
  // are features of the sealed-vault firmware (ESP32-P4 today). They are probed only when readTeacher() found that
  // vault (AI.sealedVault()), so the Cardputer and the Handheld send no extra request and show nothing; then each
  // section waits for its OWN endpoint to answer (404/405/501/any failure → it stays hidden).
  const show = (n) => { const e = $(n); if (e) e.style.display = ''; };
  async function devJson(url, body) {
    try {
      const r = await fetch(url, body ? { method: 'POST', body: JSON.stringify(body) } : { cache: 'no-store' });
      return r.ok ? await r.json() : null;
    } catch { return null; }
  }

  // ---- network mode (the device's: /api/anima/net, same as the native /mode) ----
  function paintNet(mode) {
    if (!$('net')) return;
    $('net').querySelectorAll('.it').forEach((b) => b.classList.toggle('on', b.dataset.n === mode));
    const d = t().nets.find((n) => n[0] === mode);
    if ($('nethint')) $('nethint').textContent = d ? d[2] : '';
  }
  function wireNet() {
    $('net').querySelectorAll('.it').forEach((b) => b.addEventListener('click', async () => {
      paintNet(b.dataset.n);
      const j = await devJson('/api/anima/net', { mode: b.dataset.n });
      if (j && j.mode) paintNet(j.mode);
      onChange(getCfg());
    }));
  }

  // ---- Telegram channel (/api/anima/telegram) ----
  let tgTimer = 0;
  function paintTg(j) {
    if (!j || !$('tgstat')) return;
    const b = j.bot || '?', st = t().tgst;
    const txt = j.checking ? st.checking : !j.configured ? st.none : !j.enabled ? st.off : j.paired ? st.paired : st.wait;
    $('tgstat').innerHTML = '<b>' + esc(txt.replace('%b', b)) + '</b>' + (j.error ? ' · ' + esc(j.error) : '');
    const showCode = j.configured && j.enabled && !j.paired && j.code;
    $('tgpair').style.display = showCode ? '' : 'none';
    $('tgcode').textContent = showCode ? '/pair ' + j.code : '';
    $('tgbtns').style.display = j.configured ? '' : 'none';
    $('tgon').checked = !!j.enabled;
    $('tgunlink').style.display = j.paired ? '' : 'none';
    clearTimeout(tgTimer);
    if (j.checking || showCode) tgTimer = setTimeout(() => { if (root.isConnected) tgCall(); }, j.checking ? 1500 : 4000);   // follow the check / the pairing
  }
  async function tgCall(body) { const j = await devJson('/api/anima/telegram', body); if (j) paintTg(j); }
  function wireTg() {
    $('tgsave').addEventListener('click', () => { const v = $('tgtoken').value.trim(); if (v) { $('tgtoken').value = ''; tgCall({ token: v }); } });
    $('tgon').addEventListener('change', () => tgCall({ enabled: $('tgon').checked }));
    $('tgunlink').addEventListener('click', () => tgCall({ unlink: true }));
    $('tgforget').addEventListener('click', () => { if (confirm(t().tgforget + '?')) tgCall({ forget: true }); });
    $('tgcopy').addEventListener('click', () => { try { navigator.clipboard.writeText($('tgcode').textContent); } catch {} });
  }

  // ---- the workspace (OpenClaw-style files on the SD), heartbeat interval, model permissions ----
  const WS_DIR = '/data/anima/';
  const PERM_TOOLS = ['sh', 'write', 'rule', 'open_app', 'close_app', 'set_volume', 'set_brightness', 'add_event', 'create_file', 'remember'];
  const PERM_DEF = { add_event: 'ask', create_file: 'ask', sh: 'ask', write: 'ask', rule: 'ask' };
  let wsFile = 'SOUL.md', perms = {};
  async function wsLoad(f) {
    wsFile = f;
    $('wsfiles').querySelectorAll('.it').forEach((b) => b.classList.toggle('on', b.dataset.f === f));
    $('wsdesc').textContent = t().wsdesc[f]; $('wsstat').textContent = '';
    $('wstext').value = ''; $('wstext').placeholder = t().wsempty;
    try { const txt = await fs.readText(WS_DIR + f); if (txt != null) $('wstext').value = txt; } catch {}
  }
  async function wsWrite(path, text) {
    try { await fs.write(path, text); return true; } catch (e) { return e && e.code === 'denied' ? 'unpaired' : false; }
  }
  async function savePerms() {
    const r = await wsWrite(WS_DIR + 'permissions.json', JSON.stringify(perms, null, 1));
    $('wsstat').textContent = r === true ? 'permissions.json: ' + t().wssaved : (r === 'unpaired' ? t().pair : t().cantread);
  }
  function paintPerms() {
    $('permauto').checked = perms.mode === 'auto';
    $('permauto').onchange = () => { if ($('permauto').checked) perms.mode = 'auto'; else delete perms.mode; savePerms(); };
    $('perms').innerHTML = PERM_TOOLS.map((k) => `<span>${esc(t().permnames[k])}</span><select data-p="${k}">${['allow', 'ask', 'deny'].map((v) => `<option value="${v}"${(perms[k] || PERM_DEF[k] || 'allow') === v ? ' selected' : ''}>${esc(t().permlv[v])}</option>`).join('')}</select>`).join('');
    $('perms').querySelectorAll('select').forEach((sel) => sel.addEventListener('change', () => { perms[sel.dataset.p] = sel.value; savePerms(); }));
  }
  function paintHb(j) {
    if (!j) return;
    $('hbevery').value = String(j.every);
    $('hbnext').textContent = j.every > 0 ? t().hbnext(j.next) : '';
  }
  async function hbLoad(body) { paintHb(await devJson('/api/anima/hb', body)); }
  function wireWs() {
    $('wsfiles').querySelectorAll('.it').forEach((b) => b.addEventListener('click', () => wsLoad(b.dataset.f)));
    $('wssave').addEventListener('click', async () => {
      const r = await wsWrite(WS_DIR + wsFile, $('wstext').value);
      $('wsstat').textContent = r === true ? wsFile + ': ' + t().wssaved : (r === 'unpaired' ? t().pair : t().cantread);
      if (wsFile === 'HEARTBEAT.md') hbLoad();
    });
    $('wsex').addEventListener('click', () => { if (!$('wstext').value.trim() || confirm(t().wsex + '?')) $('wstext').value = t().wsexample[wsFile]; });
    $('hbevery').addEventListener('change', () => hbLoad({ every: +$('hbevery').value }));
    (async () => {
      try { perms = (await fs.readJSON(WS_DIR + 'permissions.json', {})) || {}; } catch { perms = {}; }
      paintPerms();
    })();
    wsLoad('SOUL.md');
  }

  // ---- hands-free voice (the device's wake word, /api/anima/wake) ----
  let wakeTimer = 0;
  function paintWake(w) {
    if (!w || !$('wakestat')) return;
    const st = t().wakest[w.state] || w.state;
    $('wakestat').innerHTML = '<b>' + esc(st) + '</b>' + (w.state === 'listening' && w.label ? ' · «' + esc(w.label) + '»' : '') +
      (w.reason ? ' · ' + esc(w.reason) : '') + (w.triggers || w.on ? ' · ' + esc(t().waketrig(w.triggers || 0, w.last)) : '');
    const built = (w.words || []).length > 0 || w.state !== 'unavailable';
    $('wakeon').checked = !!w.on; $('wakeon').disabled = !built && !w.on;
    $('wakewordrow').style.display = (w.words || []).length > 1 ? '' : 'none';
    $('wakeword').innerHTML = (w.words || []).map((x) => `<option value="${esc(x.id)}"${x.id === w.word ? ' selected' : ''}>${esc(x.label)}</option>`).join('');
    $('wakesensrow').style.display = built ? '' : 'none';
    $('wakesens').querySelectorAll('.it').forEach((b) => b.classList.toggle('on', +b.dataset.s === w.sens));
    const route = (w.stt && w.stt.route) || 'none';
    $('wakenote').textContent = !built ? t().wakebuild
      : (w.label ? t().wakehint(w.label) + ' ' : '') + t().wakestt[route] + (route !== 'none' ? (w.stt.where || '') : '');
  }
  async function wakeCall(body) { const j = await devJson('/api/anima/wake?lang=' + lang, body); if (j) paintWake(j); }
  function wireWake() {
    $('wakeon').addEventListener('change', () => wakeCall({ on: $('wakeon').checked }));
    $('wakeword').addEventListener('change', () => wakeCall({ word: $('wakeword').value }));
    $('wakesens').querySelectorAll('.it').forEach((b) => b.addEventListener('click', () => wakeCall({ sens: +b.dataset.s })));
    // live while the page is visible (the wake service applies a change a moment later)
    const tick = () => { if (!root.isConnected) { clearInterval(wakeTimer); return; } if (!document.hidden) wakeCall(); };
    wakeTimer = setInterval(tick, 3000);
  }

  // ---- home Whisper server + vision helper: teacher.json fields only the sealed-vault firmware reads ----
  function wireSttVis() {
    $('sttsave').addEventListener('click', async () => {
      const u = $('stt').value.trim();
      if (u && !AI.isLanUrl(u)) { $('sttstat').textContent = t().sttbad; return; }
      cfg.stt_url = u;
      const r = await AI.writeTeacher(cfg);
      $('sttstat').textContent = r === true ? (u ? t().sttok : t().sttoff) : (r === 'unpaired' ? t().pair : t().cantread);
    });
    $('vissave').addEventListener('click', async () => {
      cfg.vision_model = $('vismodel').value.trim();
      const r = await AI.writeTeacher(cfg);
      $('visstat').textContent = r === true ? (cfg.vision_model ? t().visok : t().visoff) : (r === 'unpaired' ? t().pair : t().cantread);
    });
  }

  async function deviceSections() {
    if (!full || !AI.sealedVault()) return;              // Cardputer / Handheld: no request, nothing shown
    const [net, tg, hb, wake] = await Promise.all([devJson('/api/anima/net'), devJson('/api/anima/telegram'), devJson('/api/anima/hb'), devJson('/api/anima/wake?lang=' + lang)]);
    if (!root.isConnected) return;
    if (net && net.mode) { show('netgrp'); wireNet(); paintNet(net.mode); }
    if (tg) { show('tg'); wireTg(); paintTg(tg); }
    if (hb) { show('ws'); wireWs(); paintHb(hb); }
    if (wake) { show('wake'); wireWake(); paintWake(wake); }
    show('sttsec'); show('vissec'); wireSttVis();
  }

  // ---- the browser GPU model (WebGPU / WebLLM): the BROWSER's, so offered on every edition where WebGPU exists ----
  let gpuList = [];
  const gpuOut = (txt) => { $('gpuout').style.display = txt ? '' : 'none'; $('gpuout').textContent = txt || ''; };
  const gpuBar = (f) => { $('gpubar').style.display = f == null ? 'none' : ''; $('gpubar').firstChild.style.width = Math.round((f || 0) * 100) + '%'; };
  function gpuPaint() {
    const sel = $('gpumodel'), cur = GPU.chosen();
    sel.innerHTML = gpuList.map((m) => `<option value="${esc(m.id)}"${m.fits ? '' : ' disabled'}>${esc(m.label)} · ${m.vramMB ? (m.vramMB / 1024).toFixed(1) + ' GB' : '?'}${m.cached ? ' · ' + esc(t().gpucached) : ''}${m.fits ? '' : ' · ' + esc(t().gpubig)}</option>`).join('');
    const def = gpuList.find((m) => m.id === cur) || gpuList.find((m) => m.cached && m.fits) || gpuList.find((m) => m.fits && m.small) || gpuList.find((m) => m.fits);
    if (def) sel.value = def.id;
    $('gpuuse').checked = (() => { try { return localStorage.getItem('anima.useWebGPU') === '1'; } catch { return false; } })();
  }
  async function gpuInit() {
    if (!full || !$('gpu') || typeof navigator === 'undefined' || !('gpu' in navigator)) return;   // no WebGPU: stays hidden
    const caps = await GPU.probe().catch(() => null);
    if (!caps || !caps.webgpu || !root.isConnected) return;
    show('gpu');
    $('gpustat').textContent = 'GPU: ' + (caps.adapter || 'WebGPU') + (caps.vramMB ? ' · ~' + (caps.vramMB / 1024).toFixed(1) + ' GB' : '') + (GPU.ready() ? ' · ' + t().gpuready + GPU.loaded() : '');
    try { gpuList = await GPU.catalogue(); } catch (e) { $('gpustat').textContent += ' · ' + String(e.message || e); return; }
    ['gpurow', 'gpubtns', 'gpuuserow'].forEach((n) => { $(n).style.display = ''; });
    gpuPaint();
  }
  if (full && $('gpu')) {
    $('gpuload').addEventListener('click', async () => {
      const id = $('gpumodel').value; if (!id) return;
      gpuOut(''); gpuBar(0);
      try {
        await GPU.load(id, (p) => { gpuBar(p.progress); $('gpustat').textContent = p.text || ''; });
        gpuBar(null); $('gpustat').textContent = t().gpuready + id;
        gpuList = await GPU.catalogue(); gpuPaint();
      } catch (e) { gpuBar(null); $('gpustat').textContent = '✗ ' + String(e.message || e); }
    });
    $('gputest').addEventListener('click', async () => {
      gpuOut('…');
      try {
        if (!GPU.ready() || GPU.loaded() !== $('gpumodel').value) await GPU.load($('gpumodel').value, (p) => gpuBar(p.progress));
        gpuBar(null);
        const t0 = Date.now();
        const txt = await GPU.chat([{ role: 'user', content: t().gpuhello }], { maxTokens: 40, onToken: (_, all) => gpuOut(all) });
        gpuOut(txt + '\n(' + ((Date.now() - t0) / 1000).toFixed(1) + ' s)');
      } catch (e) { gpuBar(null); gpuOut('✗ ' + String(e.message || e)); }
    });
    $('gpuunload').addEventListener('click', async () => { await GPU.unload(); gpuInit(); });
    $('gpudel').addEventListener('click', async () => {
      const id = $('gpumodel').value; if (!id || !confirm(t().gpudel + ' ' + id + '?')) return;
      try { await GPU.remove(id); } catch {}
      gpuList = await GPU.catalogue().catch(() => gpuList); gpuPaint();
    });
    $('gpumodel').addEventListener('change', () => GPU.choose($('gpumodel').value));
    $('gpuuse').addEventListener('change', () => { try { localStorage.setItem('anima.useWebGPU', $('gpuuse').checked ? '1' : '0'); } catch {} onChange(getCfg()); });
  }

  // ---- wire ----
  $('refresh').addEventListener('click', refreshModels);
  if ($('base')) $('base').addEventListener('change', () => { cfg.base = $('base').value.trim(); cfg.liveModels = null; });
  $('save').addEventListener('click', save);
  $('test').addEventListener('click', test);
  $('del').addEventListener('click', del);
  $('key').addEventListener('keydown', (e) => { if (e.key === 'Enter') { e.preventDefault(); save(); } });
  if ($('model')) $('model').addEventListener('change', () => { cfg.model = $('model').value; });
  if ($('detect')) $('detect').addEventListener('click', () => { setStat(t().checking); calibrate(); });
  if ($('exec')) $('exec').querySelectorAll('.it').forEach((b) => b.addEventListener('click', () => {
    cfg.exec = b.dataset.x === 'device' ? 'device' : 'browser'; paint();
    AI.writeTeacher(cfg).then(() => onChange(getCfg()));   // exec is a teacher.json field — persist immediately
  }));

  paint();
  reload().then(deviceSections);   // the first vault read decides sealedVault(), the gate of the device sections
  gpuInit();

  return {
    reload, getCfg,
    setLang(l) { lang = ['it', 'es', 'fr', 'de'].includes(l) ? l : 'en'; /* re-render labels by rebuilding */ mountKeyManager(container, Object.assign({}, opts, { lang })); },
    destroy() { container.innerHTML = ''; },
  };
}
