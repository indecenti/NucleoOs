// nlcommand.js — Paint's natural-language command parser. Type a phrase, Paint does it. DETERMINISTIC and
// OFFLINE (no LLM, zero hallucination — matches the project's grounded ethos): it maps your words onto the
// editing operations that already exist (the CMD map + imaging.js) and onto the Atelier for generation.
// If it doesn't understand, it says so and suggests — it NEVER fabricates an action. Bilingual IT/EN.
// Pure & DOM-free → host-tested (tools/anima-host/paint-nlcommand.test.mjs). Paint executes the descriptor.
//
// parseCommand(text, lang) -> descriptor:
//   { kind:'edit',     op, ...args, reply }        a known editing op (rotate/removeBg/adjust/layer/…)
//   { kind:'generate', prompt, style, reply }       an image/icon/logo/background request → the Atelier
//   { kind:'unknown',  reply }                      honest miss (suggests examples), NEVER invents

const COLORS = { rosso:'#ed1c24', red:'#ed1c24', blu:'#00a2e8', blue:'#00a2e8', verde:'#22b14c', green:'#22b14c',
  giallo:'#fff200', yellow:'#fff200', nero:'#000000', black:'#000000', bianco:'#ffffff', white:'#ffffff',
  arancione:'#ff7f27', arancio:'#ff7f27', orange:'#ff7f27', viola:'#a349a4', purple:'#a349a4', magenta:'#ed1c24',
  rosa:'#ffaec9', pink:'#ffaec9', grigio:'#7f7f7f', gray:'#7f7f7f', grey:'#7f7f7f', azzurro:'#99d9ea', cyan:'#00a2e8',
  marrone:'#b97a57', brown:'#b97a57' };

function norm(s){ return ' ' + String(s == null ? '' : s).toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '')
  .replace(/['’`]/g, ' ').replace(/[^a-z0-9%+\-., x]+/g, ' ').replace(/\s+/g, ' ').trim() + ' '; }
const hasAny = (n, ...ws) => ws.some(w => n.includes(' ' + w + ' '));
const hasPhrase = (n, p) => n.includes(' ' + p + ' ');
function firstColor(n){ for (const k in COLORS) if (n.includes(' ' + k + ' ') || n.includes(' ' + k + ',')) return { name:k, hex:COLORS[k] }; return null; }
function numIn(n){ const m = n.match(/-?\d+(?:\.\d+)?/); return m ? parseFloat(m[0]) : null; }
function sizeIn(n){ const m = n.match(/(\d{2,4})\s*[x×]\s*(\d{2,4})/); return m ? { w:+m[1], h:+m[2] } : null; }

const TXT = {
  it: { empty:'Scrivi un comando.', did:(s)=>s, gen:(p)=>`Genero: ${p}`,
    unknown:'Non ho capito. Prova: "rimuovi lo sfondo", "ruota a destra", "luminosità +20", "nuovo livello", "crea un\'icona di un gatto".',
    names:{ rotcw:'Ruoto a destra', rotccw:'Ruoto a sinistra', fliph:'Rifletto in orizzontale', flipv:'Rifletto in verticale',
      removeBg:'Rimuovo lo sfondo', fxGray:'Scala di grigi', fxInvert:'Inverto i colori', fxSepia:'Applico il seppia',
      fxBlur:'Sfoco', fxSharp:'Aumento la nitidezza', fxEdges:'Contorni', trim:'Rifilo i bordi trasparenti',
      cropSel:'Ritaglio alla selezione', layerAdd:'Nuovo livello', layerDup:'Duplico il livello', layerMerge:'Unisco sotto',
      layerFlatten:'Appiattisco', layerDel:'Elimino il livello', clear:'Svuoto il livello', undo:'Annullo', redo:'Ripeto',
      save:'Salvo', deselect:'Deseleziono', toggleGrid:'Griglia', toggleRulers:'Righelli', fitZoom:'Adatto alla finestra',
      zoomReset:'Zoom 100%', newTransparent:'Nuova tela trasparente' } },
  en: { empty:'Type a command.', did:(s)=>s, gen:(p)=>`Generating: ${p}`,
    unknown:'I didn\'t get that. Try: "remove the background", "rotate right", "brightness +20", "new layer", "create an icon of a cat".',
    names:{ rotcw:'Rotate right', rotccw:'Rotate left', fliph:'Flip horizontal', flipv:'Flip vertical',
      removeBg:'Remove background', fxGray:'Grayscale', fxInvert:'Invert colors', fxSepia:'Sepia', fxBlur:'Blur',
      fxSharp:'Sharpen', fxEdges:'Edges', trim:'Trim transparent edges', cropSel:'Crop to selection', layerAdd:'New layer',
      layerDup:'Duplicate layer', layerMerge:'Merge down', layerFlatten:'Flatten', layerDel:'Delete layer', clear:'Clear layer',
      undo:'Undo', redo:'Redo', save:'Save', deselect:'Deselect', toggleGrid:'Grid', toggleRulers:'Rulers',
      fitZoom:'Fit to window', zoomReset:'Zoom 100%', newTransparent:'New transparent canvas' } },
};
const L = (lang) => TXT[lang === 'en' ? 'en' : 'it'];
const edit = (lang, op, extra = {}) => ({ kind:'edit', op, ...extra, reply: L(lang).names[op] || op });

export function parseCommand(text, lang = 'it') {
  const t = L(lang), n = norm(text);
  if (!n.trim()) return { kind:'unknown', reply:t.empty };

  // ---- LAYERS (checked before generate so "crea un livello" ≠ generate) ----
  if (hasAny(n,'appiattisci','appiattire','flatten') || hasPhrase(n,'unisci tutto') || hasPhrase(n,'unisci tutti i livelli')) return edit(lang,'layerFlatten');
  if (hasPhrase(n,'unisci sotto') || hasPhrase(n,'unisci sotto il livello') || hasPhrase(n,'merge down') || (hasAny(n,'unisci','merge') && hasAny(n,'livello','livelli','layer'))) return edit(lang,'layerMerge');
  if (hasAny(n,'duplica','duplicare','duplicate') && hasAny(n,'livello','layer')) return edit(lang,'layerDup');
  if ((hasAny(n,'elimina','eliminare','cancella','rimuovi','delete','remove') && hasAny(n,'livello','layer'))) return edit(lang,'layerDel');
  if ((hasAny(n,'nuovo','aggiungi','crea','add','new','create') && hasAny(n,'livello','layer'))) return edit(lang,'layerAdd');

  // ---- TRANSFORMS ----
  if (hasAny(n,'ruota','ruotare','gira','girare','rotate','rotazione') ) {
    if (hasAny(n,'sinistra','antiorario','antioraria','left','ccw')) return edit(lang,'rotccw');
    return edit(lang,'rotcw');
  }
  if (hasAny(n,'capovolgi','ribalta','rifletti','specchia','flip','mirror')) {
    if (hasAny(n,'verticale','verticalmente','vertical','sotto','sopra')) return edit(lang,'flipv');
    return edit(lang,'fliph');
  }
  if (hasPhrase(n,'rifila') || hasPhrase(n,'rifila i bordi') || hasPhrase(n,'taglia i bordi') || hasPhrase(n,'trim') || hasPhrase(n,'trim edges') || hasPhrase(n,'rimuovi i bordi trasparenti')) return edit(lang,'trim');
  if ((hasAny(n,'ritaglia','ritagliare','crop') && hasAny(n,'selezione','selection'))) return edit(lang,'cropSel');
  const sz = sizeIn(n);
  if (sz && hasAny(n,'ridimensiona','ridimensionare','resize','scala','imposta')) return { kind:'edit', op:'resize', w:sz.w, h:sz.h, reply:(lang==='en'?`Resize to ${sz.w}×${sz.h}`:`Ridimensiono a ${sz.w}×${sz.h}`) };

  // ---- BACKGROUND ----
  if ((hasAny(n,'rimuovi','togli','elimina','leva','scontorna','remove','delete','erase') && hasAny(n,'sfondo','background','bg')) || hasPhrase(n,'scontorna') || hasPhrase(n,'remove the background')) return edit(lang,'removeBg');

  // ---- FILTERS ----
  if (hasPhrase(n,'scala di grigi') || hasPhrase(n,'bianco e nero') || hasAny(n,'grayscale','greyscale') || hasPhrase(n,'black and white')) return edit(lang,'fxGray');
  if (hasAny(n,'inverti','invertire','negativo','invert','negative')) return edit(lang,'fxInvert');
  if (hasAny(n,'seppia','sepia')) return edit(lang,'fxSepia');
  if (hasAny(n,'sfoca','sfocare','sfocatura','blur')) return edit(lang,'fxBlur');
  if (hasAny(n,'nitidezza','nitido','sharpen','sharp')) return edit(lang,'fxSharp');
  if (hasAny(n,'contorni','bordi','edges','outline') && !hasAny(n,'rifila','trim')) return edit(lang,'fxEdges');

  // ---- ADJUSTMENTS (brightness / contrast / saturation, set or delta) ----
  let which = null;
  if (hasAny(n,'luminosita','luminosità','luminoso','brightness','bright')) which='brightness';
  else if (hasAny(n,'contrasto','contrast')) which='contrast';
  else if (hasAny(n,'saturazione','saturo','saturation','saturate')) which='saturation';
  if (which) {
    let v = numIn(n);
    const up = hasAny(n,'aumenta','alza','piu','più','more','up','increase','+') || /\+\d/.test(n);
    const down = hasAny(n,'diminuisci','abbassa','riduci','meno','less','down','decrease') || /-\d/.test(n);
    const setTo = hasAny(n,'a','to','=','imposta','metti','porta');
    let value, mode;
    if (v == null) { value = down ? -20 : 20; mode='delta'; }
    else if (setTo && !up && !down) { value = v; mode='set'; }
    else { value = down && v > 0 ? -v : v; mode='delta'; }
    const lab = { brightness: lang==='en'?'Brightness':'Luminosità', contrast: lang==='en'?'Contrast':'Contrasto', saturation: lang==='en'?'Saturation':'Saturazione' }[which];
    return { kind:'edit', op:'adjust', which, value, mode, reply:`${lab} ${mode==='set'?'= ':(value>=0?'+':'')}${value}` };
  }

  // ---- OPACITY (active layer) ----
  if (hasAny(n,'opacita','opacità','opacity','trasparenza') && (numIn(n) != null)) {
    const v = Math.max(0, Math.min(100, numIn(n))); return { kind:'edit', op:'opacity', value:v, reply:(lang==='en'?`Opacity ${v}%`:`Opacità ${v}%`) };
  }

  // ---- ZOOM / VIEW ----
  if (hasAny(n,'adatta','fit') && hasAny(n,'finestra','schermo','window','screen','zoom')) return edit(lang,'fitZoom');
  if (hasAny(n,'zoom','ingrandisci','rimpicciolisci','zooma')) {
    const v = numIn(n); if (v != null) return { kind:'edit', op:'zoom', value:v, reply:`Zoom ${Math.round(v)}%` };
    if (hasAny(n,'rimpicciolisci','riduci','out')) return { kind:'edit', op:'zoom', dir:'out', reply:'Zoom -' };
    return { kind:'edit', op:'zoom', dir:'in', reply:'Zoom +' };
  }
  if (hasAny(n,'griglia','grid')) return edit(lang,'toggleGrid');
  if (hasAny(n,'righelli','righello','rulers','ruler')) return edit(lang,'toggleRulers');
  if (hasAny(n,'deseleziona','deselect')) return edit(lang,'deselect');

  // ---- FILE / HISTORY ----
  if (hasAny(n,'annulla','undo')) return edit(lang,'undo');
  if (hasAny(n,'ripeti','redo')) return edit(lang,'redo');
  if (hasAny(n,'salva','save')) return edit(lang,'save');
  if ((hasAny(n,'svuota','cancella','clear') && hasAny(n,'tutto','tela','canvas','all')) || hasPhrase(n,'cancella tutto') || hasPhrase(n,'clear all')) return edit(lang,'clear');
  if (hasPhrase(n,'tela trasparente') || hasPhrase(n,'nuova trasparente') || hasPhrase(n,'transparent canvas')) return edit(lang,'newTransparent');

  // ---- FILL with a colour ----
  const col = firstColor(n);
  if (col && hasAny(n,'riempi','riempire','colora','riempimento','fill','sfondo')) return { kind:'edit', op:'fill', color:col.hex, reply:(lang==='en'?`Fill ${col.name}`:`Riempio di ${col.name}`) };

  // ---- GENERATE (image / icon / logo / background / drawing) ----
  const g = tryGenerate(n, text, lang);
  if (g) return g;

  return { kind:'unknown', reply:t.unknown };
}

const GEN_VERBS = ['genera','generami','generarmi','generare','crea','creami','crearmi','creare','disegna','disegnami','disegnarmi','disegnare','dipingi','dipingimi','dipingere','raffigura','raffigurami','produci','generate','create','draw','make','render','paint','produce','sketch'];
const NOUN_STYLE = { icona:'icon', icone:'icon', icon:'icon', icons:'icon', logo:'logo', logos:'logo',
  sfondo:'background', wallpaper:'background', background:'background', paesaggio:'background', landscape:'background',
  immagine:'image', immagini:'image', image:'image', images:'image', foto:'photo', fotografia:'photo', photo:'photo', photograph:'photo',
  disegno:'drawing', disegni:'drawing', drawing:'drawing', illustrazione:'illustration', illustration:'illustration',
  ritratto:'portrait', portrait:'portrait', quadro:'painting', dipinto:'painting', painting:'painting', artwork:'artwork' };

function tryGenerate(n, raw, lang) {
  const hasVerb = GEN_VERBS.some(v => n.includes(' ' + v + ' '));
  let noun = null; for (const k in NOUN_STYLE) if (n.includes(' ' + k + ' ')) { noun = k; break; }
  // request forms ("disegnami X" / "draw me X") don't need an image-noun
  const reqForm = hasAny(n,'disegnami','disegnarmi','dipingimi','raffigurami') || (hasAny(n,'draw','paint','sketch') && hasAny(n,'me'));
  if (!hasVerb && !noun) return null;
  if (!noun && !reqForm) return null;                       // a bare gen-verb without an object isn't a generate
  const style = noun ? NOUN_STYLE[noun] : 'image';
  // extract the subject prompt: text AFTER "noun di/of …", else after the verb, stripped of lead-in words.
  let p = norm(raw);
  const cut = (marker) => { const i = p.indexOf(' ' + marker + ' '); if (i >= 0) p = p.slice(i + marker.length + 2); };
  if (noun) { cut(noun); } else { for (const v of GEN_VERBS) { const i = p.indexOf(' ' + v + ' '); if (i >= 0) { p = p.slice(i + v.length + 2); break; } } }
  p = ' ' + p.trim() + ' ';
  for (const lead of [' di ',' of ',' del ',' della ',' dello ',' per ',' for ',' a ',' an ',' the ',' un ',' uno ',' una ',' che rappresenta ',' che raffigura ',' raffigurante ',' con ']) { if (p.startsWith(lead)) { p = ' ' + p.slice(lead.length); } }
  for (const art of ['un ','uno ','una ','il ','lo ','la ','i ','gli ','le ','a ','an ','the ']) { if (p.trim().startsWith(art)) { p = ' ' + p.trim().slice(art.length); } }
  p = p.trim();
  if (!p) p = (style === 'icon' ? (lang==='en'?'a simple icon':'un\'icona semplice') : (lang==='en'?'an abstract image':'un\'immagine astratta'));
  return { kind:'generate', prompt:p, style, reply: L(lang).gen(p) };
}

// Build the final Atelier prompt + flags from a generate descriptor (style-aware: icons/logos get a flat,
// centred, plain-background prompt so the auto background-removal yields a clean transparent asset).
export function buildGeneration(desc, lang = 'it') {
  const p = desc.prompt;
  if (desc.style === 'icon') return { prompt: `${p}, flat icon, simple, vector style, centered, solid white background, minimal`, removeBg:true, negative:'photo, realistic, busy background, text, watermark' };
  if (desc.style === 'logo') return { prompt: `${p}, logo, flat, minimal, vector, centered, plain background`, removeBg:true, negative:'photo, realistic, busy, text, watermark' };
  if (desc.style === 'background') return { prompt: `${p}, wide scenic background, detailed`, removeBg:false, negative:'text, watermark, frame' };
  if (desc.style === 'photo') return { prompt: `${p}, photorealistic, detailed`, removeBg:false, negative:'cartoon, lowres, watermark, text' };
  if (desc.style === 'painting') return { prompt: `${p}, painting, artistic`, removeBg:false, negative:'lowres, watermark, text' };
  return { prompt: p, removeBg:false, negative:'lowres, watermark, text, deformed' };
}
