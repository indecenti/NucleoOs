// nlcommand.js — Paint's natural-language command parser. Type a phrase, Paint does it. DETERMINISTIC and
// OFFLINE (no LLM, zero hallucination — matches the project's grounded ethos): it maps your words onto the
// editing operations that already exist (the CMD map + imaging.js) and onto the Atelier for generation.
// If it doesn't understand, it says so and suggests — it NEVER fabricates an action. Five languages
// (it/en/es/fr/de, like the OS): until 2026-10 only Italian and English were understood, and every other UI
// language got its replies in Italian.
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
  marrone:'#b97a57', brown:'#b97a57',
  // es / fr / de (accent-folded like norm(); "weiß" → "weiss")
  rojo:'#ed1c24', azul:'#00a2e8', amarillo:'#fff200', negro:'#000000', blanco:'#ffffff', naranja:'#ff7f27', morado:'#a349a4', gris:'#7f7f7f', marron:'#b97a57',
  rouge:'#ed1c24', bleu:'#00a2e8', vert:'#22b14c', jaune:'#fff200', noir:'#000000', blanc:'#ffffff', violet:'#a349a4', rose:'#ffaec9',
  rot:'#ed1c24', blau:'#00a2e8', grun:'#22b14c', gelb:'#fff200', schwarz:'#000000', weiss:'#ffffff', lila:'#a349a4', grau:'#7f7f7f', braun:'#b97a57' };

function norm(s){ return ' ' + String(s == null ? '' : s).toLowerCase().replace(/ß/g, 'ss').normalize('NFD').replace(/[̀-ͯ]/g, '')
  .replace(/['’`]/g, ' ').replace(/[^a-z0-9%+\-., x]+/g, ' ').replace(/\s+/g, ' ').trim() + ' '; }
const hasAny = (n, ...ws) => ws.some(w => n.includes(' ' + w + ' '));
const hasPhrase = (n, p) => n.includes(' ' + p + ' ');
function firstColor(n){ for (const k in COLORS) if (n.includes(' ' + k + ' ') || n.includes(' ' + k + ',')) return { name:k, hex:COLORS[k] }; return null; }
function numIn(n){ const m = n.match(/-?\d+(?:\.\d+)?/); return m ? parseFloat(m[0]) : null; }
function sizeIn(n){ const m = n.match(/(\d{2,4})\s*[x×]\s*(\d{2,4})/); return m ? { w:+m[1], h:+m[2] } : null; }

const TXT = {
  it: { empty:'Scrivi un comando.', did:(s)=>s, gen:(p)=>`Genero: ${p}`,
    unknown:'Non ho capito. Prova: "rimuovi lo sfondo", "ruota a destra", "luminosità +20", "nuovo livello", "crea un\'icona di un gatto".',
    resize:(w,h)=>`Ridimensiono a ${w}×${h}`, opacity:(v)=>`Opacità ${v}%`, fill:(c)=>`Riempio di ${c}`, adj:{ brightness:'Luminosità', contrast:'Contrasto', saturation:'Saturazione' },
    names:{ rotcw:'Ruoto a destra', rotccw:'Ruoto a sinistra', fliph:'Rifletto in orizzontale', flipv:'Rifletto in verticale',
      removeBg:'Rimuovo lo sfondo', fxGray:'Scala di grigi', fxInvert:'Inverto i colori', fxSepia:'Applico il seppia',
      fxBlur:'Sfoco', fxSharp:'Aumento la nitidezza', fxEdges:'Contorni', trim:'Rifilo i bordi trasparenti',
      cropSel:'Ritaglio alla selezione', layerAdd:'Nuovo livello', layerDup:'Duplico il livello', layerMerge:'Unisco sotto',
      layerFlatten:'Appiattisco', layerDel:'Elimino il livello', clear:'Svuoto il livello', undo:'Annullo', redo:'Ripeto',
      save:'Salvo', deselect:'Deseleziono', toggleGrid:'Griglia', toggleRulers:'Righelli', fitZoom:'Adatto alla finestra',
      zoomReset:'Zoom 100%', newTransparent:'Nuova tela trasparente' } },
  en: { empty:'Type a command.', did:(s)=>s, gen:(p)=>`Generating: ${p}`,
    unknown:'I didn\'t get that. Try: "remove the background", "rotate right", "brightness +20", "new layer", "create an icon of a cat".',
    resize:(w,h)=>`Resize to ${w}×${h}`, opacity:(v)=>`Opacity ${v}%`, fill:(c)=>`Fill ${c}`, adj:{ brightness:'Brightness', contrast:'Contrast', saturation:'Saturation' },
    names:{ rotcw:'Rotate right', rotccw:'Rotate left', fliph:'Flip horizontal', flipv:'Flip vertical',
      removeBg:'Remove background', fxGray:'Grayscale', fxInvert:'Invert colors', fxSepia:'Sepia', fxBlur:'Blur',
      fxSharp:'Sharpen', fxEdges:'Edges', trim:'Trim transparent edges', cropSel:'Crop to selection', layerAdd:'New layer',
      layerDup:'Duplicate layer', layerMerge:'Merge down', layerFlatten:'Flatten', layerDel:'Delete layer', clear:'Clear layer',
      undo:'Undo', redo:'Redo', save:'Save', deselect:'Deselect', toggleGrid:'Grid', toggleRulers:'Rulers',
      fitZoom:'Fit to window', zoomReset:'Zoom 100%', newTransparent:'New transparent canvas' } },
  es: { empty:'Escribe una orden.', did:(s)=>s, gen:(p)=>`Genero: ${p}`,
    unknown:'No lo he entendido. Prueba: "quita el fondo", "gira a la derecha", "brillo +20", "nueva capa", "crea un icono de un gato".',
    resize:(w,h)=>`Redimensiono a ${w}×${h}`, opacity:(v)=>`Opacidad ${v} %`, fill:(c)=>`Relleno de ${c}`, adj:{ brightness:'Brillo', contrast:'Contraste', saturation:'Saturación' },
    names:{ rotcw:'Giro a la derecha', rotccw:'Giro a la izquierda', fliph:'Volteo en horizontal', flipv:'Volteo en vertical',
      removeBg:'Quito el fondo', fxGray:'Escala de grises', fxInvert:'Invierto los colores', fxSepia:'Aplico sepia', fxBlur:'Desenfoco',
      fxSharp:'Aumento la nitidez', fxEdges:'Bordes', trim:'Recorto los bordes transparentes', cropSel:'Recorto a la selección', layerAdd:'Nueva capa',
      layerDup:'Duplico la capa', layerMerge:'Combino hacia abajo', layerFlatten:'Acoplo la imagen', layerDel:'Elimino la capa', clear:'Vacío la capa',
      undo:'Deshago', redo:'Rehago', save:'Guardo', deselect:'Quito la selección', toggleGrid:'Cuadrícula', toggleRulers:'Reglas',
      fitZoom:'Ajusto a la ventana', zoomReset:'Zoom 100 %', newTransparent:'Nuevo lienzo transparente' } },
  fr: { empty:'Écris une commande.', did:(s)=>s, gen:(p)=>`Je génère : ${p}`,
    unknown:'Je n’ai pas compris. Essaie : « supprime l’arrière-plan », « tourne à droite », « luminosité +20 », « nouveau calque », « crée une icône de chat ».',
    resize:(w,h)=>`Redimensionne à ${w}×${h}`, opacity:(v)=>`Opacité ${v} %`, fill:(c)=>`Remplis de ${c}`, adj:{ brightness:'Luminosité', contrast:'Contraste', saturation:'Saturation' },
    names:{ rotcw:'Je tourne à droite', rotccw:'Je tourne à gauche', fliph:'Miroir horizontal', flipv:'Miroir vertical',
      removeBg:'Je supprime l’arrière-plan', fxGray:'Niveaux de gris', fxInvert:'J’inverse les couleurs', fxSepia:'Sépia', fxBlur:'Flou',
      fxSharp:'Plus de netteté', fxEdges:'Contours', trim:'Je rogne les bords transparents', cropSel:'Recadrage sur la sélection', layerAdd:'Nouveau calque',
      layerDup:'Je duplique le calque', layerMerge:'Fusion vers le bas', layerFlatten:'J’aplatis l’image', layerDel:'Je supprime le calque', clear:'Je vide le calque',
      undo:'J’annule', redo:'Je rétablis', save:'J’enregistre', deselect:'Je désélectionne', toggleGrid:'Grille', toggleRulers:'Règles',
      fitZoom:'Ajusté à la fenêtre', zoomReset:'Zoom 100 %', newTransparent:'Nouvelle toile transparente' } },
  de: { empty:'Gib einen Befehl ein.', did:(s)=>s, gen:(p)=>`Ich erzeuge: ${p}`,
    unknown:'Das habe ich nicht verstanden. Versuch: „Hintergrund entfernen“, „nach rechts drehen“, „Helligkeit +20“, „neue Ebene“, „erstelle ein Symbol einer Katze“.',
    resize:(w,h)=>`Größe ${w}×${h}`, opacity:(v)=>`Deckkraft ${v} %`, fill:(c)=>`Fülle mit ${c}`, adj:{ brightness:'Helligkeit', contrast:'Kontrast', saturation:'Sättigung' },
    names:{ rotcw:'Nach rechts gedreht', rotccw:'Nach links gedreht', fliph:'Horizontal gespiegelt', flipv:'Vertikal gespiegelt',
      removeBg:'Hintergrund entfernt', fxGray:'Graustufen', fxInvert:'Farben invertiert', fxSepia:'Sepia', fxBlur:'Weichgezeichnet',
      fxSharp:'Geschärft', fxEdges:'Kanten', trim:'Transparente Ränder beschnitten', cropSel:'Auf Auswahl zugeschnitten', layerAdd:'Neue Ebene',
      layerDup:'Ebene dupliziert', layerMerge:'Nach unten zusammengefügt', layerFlatten:'Auf eine Ebene reduziert', layerDel:'Ebene gelöscht', clear:'Ebene geleert',
      undo:'Rückgängig', redo:'Wiederholt', save:'Gespeichert', deselect:'Auswahl aufgehoben', toggleGrid:'Raster', toggleRulers:'Lineale',
      fitZoom:'An Fenster angepasst', zoomReset:'Zoom 100 %', newTransparent:'Neue transparente Leinwand' } },
};
const L = (lang) => TXT[lang] || TXT.en;
const edit = (lang, op, extra = {}) => ({ kind:'edit', op, ...extra, reply: L(lang).names[op] || op });

// Shared vocabulary (accent-folded, five languages).
const LAYER = ['livello','livelli','layer','layers','capa','capas','calque','calques','ebene','ebenen'];
const W_DEL = ['elimina','eliminare','cancella','rimuovi','delete','remove','eliminar','borra','borrar','quita','quitar','supprime','supprimer','efface','effacer','enleve','enlever','retire','retirer','losche','loschen','entferne','entfernen'];
const W_NEW = ['nuovo','aggiungi','crea','add','new','create','nueva','nuevo','anade','anadir','agrega','agregar','crear','nouveau','nouvelle','ajoute','ajouter','cree','creer','neue','neues','neuen','fuge','hinzufugen','erstelle','erstellen'];
const W_UP = ['aumenta','alza','piu','più','more','up','increase','+','sube','subir','mas','augmente','augmenter','plus','erhohe','erhohen','mehr','heller'];
const W_DOWN = ['diminuisci','abbassa','riduci','meno','less','down','decrease','baja','bajar','reduce','reducir','disminuye','menos','baisse','baisser','diminue','reduis','moins','verringere','senke','reduziere','weniger','dunkler'];

export function parseCommand(text, lang = 'it') {
  const t = L(lang), n = norm(text);
  if (!n.trim()) return { kind:'unknown', reply:t.empty };

  // ---- LAYERS (checked before generate so "crea un livello" ≠ generate) ----
  if (hasAny(n,'appiattisci','appiattire','flatten','aplana','aplanar','acopla','acoplar','aplatis','aplatir') || hasPhrase(n,'unisci tutto') || hasPhrase(n,'unisci tutti i livelli') || hasPhrase(n,'auf eine ebene reduzieren') || (hasAny(n,'reduziere','reduzieren') && hasAny(n,'ebene','ebenen'))) return edit(lang,'layerFlatten');
  if (hasPhrase(n,'unisci sotto') || hasPhrase(n,'unisci sotto il livello') || hasPhrase(n,'merge down') || hasPhrase(n,'combinar hacia abajo') || hasPhrase(n,'fusionner vers le bas') || hasPhrase(n,'nach unten zusammenfugen') ||
      (hasAny(n,'unisci','merge','combina','combinar','fusiona','fusionar','fusionne','fusionner','zusammenfugen','vereine','vereinen') && hasAny(n, ...LAYER))) return edit(lang,'layerMerge');
  if (hasAny(n,'duplica','duplicare','duplicate','duplicar','duplique','dupliquer','dupliziere','duplizieren') && hasAny(n, ...LAYER)) return edit(lang,'layerDup');
  if (hasAny(n, ...W_DEL) && hasAny(n, ...LAYER)) return edit(lang,'layerDel');
  if (hasAny(n, ...W_NEW) && hasAny(n, ...LAYER)) return edit(lang,'layerAdd');

  // ---- TRANSFORMS ----
  if (hasAny(n,'ruota','ruotare','gira','girare','rotate','rotazione','girar','rota','rotar','tourne','tourner','pivote','pivoter','rotation','drehe','drehen','dreh','rotiere','rotieren') ) {
    if (hasAny(n,'sinistra','antiorario','antioraria','left','ccw','izquierda','gauche','links')) return edit(lang,'rotccw');
    return edit(lang,'rotcw');
  }
  if (hasAny(n,'capovolgi','ribalta','rifletti','specchia','flip','mirror','voltea','voltear','refleja','reflejar','espejo','retourne','retourner','miroir','spiegle','spiegeln','spiegel')) {
    if (hasAny(n,'verticale','verticalmente','vertical','sotto','sopra','verticalement','vertikal','senkrecht')) return edit(lang,'flipv');
    return edit(lang,'fliph');
  }
  if (hasPhrase(n,'rifila') || hasPhrase(n,'rifila i bordi') || hasPhrase(n,'taglia i bordi') || hasPhrase(n,'trim') || hasPhrase(n,'trim edges') || hasPhrase(n,'rimuovi i bordi trasparenti') ||
      hasPhrase(n,'recorta los bordes') || hasPhrase(n,'rogne les bords') || hasPhrase(n,'rander beschneiden')) return edit(lang,'trim');
  if ((hasAny(n,'ritaglia','ritagliare','crop','recorta','recortar','recadre','recadrer','rogne','zuschneiden','beschneide','beschneiden') && hasAny(n,'selezione','selection','seleccion','auswahl'))) return edit(lang,'cropSel');
  const sz = sizeIn(n);
  if (sz && hasAny(n,'ridimensiona','ridimensionare','resize','scala','imposta','redimensiona','redimensionar','escala','redimensionne','redimensionner','skaliere','skalieren','grosse')) return { kind:'edit', op:'resize', w:sz.w, h:sz.h, reply:t.resize(sz.w, sz.h) };

  // ---- BACKGROUND ----
  if ((hasAny(n,'rimuovi','togli','elimina','leva','scontorna','remove','delete','erase','quita','quitar','elimina','borra','supprime','enleve','retire','efface','entferne','entfernen','losche') && hasAny(n,'sfondo','background','bg','fondo','fond','arriere-plan','hintergrund')) ||
      hasPhrase(n,'scontorna') || hasPhrase(n,'remove the background') || hasPhrase(n,'hintergrund entfernen')) return edit(lang,'removeBg');

  // ---- FILTERS ----
  if (hasPhrase(n,'scala di grigi') || hasPhrase(n,'bianco e nero') || hasAny(n,'grayscale','greyscale','graustufen','schwarzweiss','schwarz-weiss') || hasPhrase(n,'black and white') ||
      hasPhrase(n,'escala de grises') || hasPhrase(n,'blanco y negro') || hasPhrase(n,'niveaux de gris') || hasPhrase(n,'noir et blanc')) return edit(lang,'fxGray');
  if (hasAny(n,'inverti','invertire','negativo','invert','negative','invierte','invertir','inverse','inverser','negatif','invertiere','invertieren','negativ')) return edit(lang,'fxInvert');
  if (hasAny(n,'seppia','sepia')) return edit(lang,'fxSepia');
  if (hasAny(n,'sfoca','sfocare','sfocatura','blur','desenfoca','desenfocar','difumina','flou','floute','flouter','weichzeichnen','verwische','verwischen','unscharf')) return edit(lang,'fxBlur');
  if (hasAny(n,'nitidezza','nitido','sharpen','sharp','nitidez','nettete','accentue','scharfe','scharfen','scharfer')) return edit(lang,'fxSharp');
  if (hasAny(n,'contorni','bordi','edges','outline','bordes','contornos','contours','bords','kanten','konturen') && !hasAny(n,'rifila','trim')) return edit(lang,'fxEdges');

  // ---- ADJUSTMENTS (brightness / contrast / saturation, set or delta) ----
  let which = null;
  if (hasAny(n,'luminosita','luminosità','luminoso','brightness','bright','brillo','luminosite','helligkeit')) which='brightness';
  else if (hasAny(n,'contrasto','contrast','contraste','kontrast')) which='contrast';
  else if (hasAny(n,'saturazione','saturo','saturation','saturate','saturacion','sattigung')) which='saturation';
  if (which) {
    let v = numIn(n);
    const up = hasAny(n, ...W_UP) || /\+\d/.test(n);
    const down = hasAny(n, ...W_DOWN) || /-\d/.test(n);
    const setTo = hasAny(n,'a','to','=','imposta','metti','porta','pon','poner','mets','mettre','auf','setze','stelle');
    let value, mode;
    if (v == null) { value = down ? -20 : 20; mode='delta'; }
    else if (setTo && !up && !down) { value = v; mode='set'; }
    else { value = down && v > 0 ? -v : v; mode='delta'; }
    return { kind:'edit', op:'adjust', which, value, mode, reply:`${t.adj[which]} ${mode==='set'?'= ':(value>=0?'+':'')}${value}` };
  }

  // ---- OPACITY (active layer) ----
  if (hasAny(n,'opacita','opacità','opacity','trasparenza','opacidad','opacite','deckkraft') && (numIn(n) != null)) {
    const v = Math.max(0, Math.min(100, numIn(n))); return { kind:'edit', op:'opacity', value:v, reply:t.opacity(v) };
  }

  // ---- ZOOM / VIEW ----
  if (hasAny(n,'adatta','fit','ajusta','ajustar','ajuste','ajuster','einpassen','anpassen') && hasAny(n,'finestra','schermo','window','screen','zoom','ventana','pantalla','fenetre','ecran','fenster','bildschirm')) return edit(lang,'fitZoom');
  if (hasAny(n,'zoom','ingrandisci','rimpicciolisci','zooma','amplia','acerca','aleja','agrandis','zoome','vergrossere','vergrossern','verkleinere','verkleinern')) {
    const v = numIn(n); if (v != null) return { kind:'edit', op:'zoom', value:v, reply:`Zoom ${Math.round(v)}%` };
    if (hasAny(n,'rimpicciolisci','riduci','out','aleja','verkleinere','verkleinern','reduis')) return { kind:'edit', op:'zoom', dir:'out', reply:'Zoom -' };
    return { kind:'edit', op:'zoom', dir:'in', reply:'Zoom +' };
  }
  if (hasAny(n,'griglia','grid','cuadricula','rejilla','grille','raster','gitter')) return edit(lang,'toggleGrid');
  if (hasAny(n,'righelli','righello','rulers','ruler','reglas','regles','lineale','lineal')) return edit(lang,'toggleRulers');
  if (hasAny(n,'deseleziona','deselect','deselecciona','deseleccionar','deselectionne','deselectionner') || hasPhrase(n,'auswahl aufheben')) return edit(lang,'deselect');

  // ---- FILE / HISTORY ----
  if (hasAny(n,'annulla','undo','deshacer','deshaz','annule','annuler','ruckgangig')) return edit(lang,'undo');
  if (hasAny(n,'ripeti','redo','rehacer','rehaz','retablis','retablir','refaire','wiederhole','wiederholen','wiederherstellen')) return edit(lang,'redo');
  // "guarda" saves only in Spanish: in Italian it means "look"
  if (hasAny(n,'salva','save','guardar','enregistre','enregistrer','sauvegarde','sauvegarder','speichere','speichern') || (lang === 'es' && hasAny(n,'guarda'))) return edit(lang,'save');
  if ((hasAny(n,'svuota','cancella','clear','borra','limpia','limpiar','efface','vide','vider','losche','leere') && hasAny(n,'tutto','tela','canvas','all','todo','lienzo','tout','toile','alles','leinwand')) ||
      hasPhrase(n,'cancella tutto') || hasPhrase(n,'clear all') || hasPhrase(n,'borra todo') || hasPhrase(n,'efface tout') || hasPhrase(n,'alles loschen')) return edit(lang,'clear');
  if (hasPhrase(n,'tela trasparente') || hasPhrase(n,'nuova trasparente') || hasPhrase(n,'transparent canvas') || hasPhrase(n,'lienzo transparente') || hasPhrase(n,'toile transparente') || hasPhrase(n,'transparente leinwand')) return edit(lang,'newTransparent');

  // ---- FILL with a colour ----
  const col = firstColor(n);
  // no bare "pinta": "pinta una rosa" is a drawing of a rose, not a pink fill ("pinta el fondo de azul" has "fondo")
  if (col && hasAny(n,'riempi','riempire','colora','riempimento','fill','sfondo','rellena','rellenar','colorea','fondo','remplis','remplir','colorie','fond','fulle','fullen','farbe','hintergrund')) return { kind:'edit', op:'fill', color:col.hex, reply:t.fill(col.name) };

  // ---- GENERATE (image / icon / logo / background / drawing) ----
  const g = tryGenerate(n, text, lang);
  if (g) return g;

  return { kind:'unknown', reply:t.unknown };
}

const GEN_VERBS = ['genera','generami','generarmi','generare','crea','creami','crearmi','creare','disegna','disegnami','disegnarmi','disegnare','dipingi','dipingimi','dipingere','raffigura','raffigurami','produci','generate','create','draw','make','render','paint','produce','sketch',
  'generar','crear','dibuja','dibujame','dibujar','pinta','pintame','haz','hazme',
  'genere','generer','cree','creer','dessine','dessine-moi','dessiner','peins','peindre','fais','fais-moi',
  'generiere','generieren','erstelle','erstellen','zeichne','zeichnen','male','malen','mach','mache'];
const NOUN_STYLE = { icona:'icon', icone:'icon', icon:'icon', icons:'icon', logo:'logo', logos:'logo',
  sfondo:'background', wallpaper:'background', background:'background', paesaggio:'background', landscape:'background',
  immagine:'image', immagini:'image', image:'image', images:'image', foto:'photo', fotografia:'photo', photo:'photo', photograph:'photo',
  disegno:'drawing', disegni:'drawing', drawing:'drawing', illustrazione:'illustration', illustration:'illustration',
  ritratto:'portrait', portrait:'portrait', quadro:'painting', dipinto:'painting', painting:'painting', artwork:'artwork',
  icono:'icon', logotipo:'logo', fondo:'background', paisaje:'background', imagen:'image', dibujo:'drawing', ilustracion:'illustration', retrato:'portrait', cuadro:'painting', pintura:'painting',
  'fond d ecran':'background', paysage:'background', dessin:'drawing', tableau:'painting', peinture:'painting',
  symbol:'icon', hintergrund:'background', landschaft:'background', bild:'image', zeichnung:'drawing', portrat:'portrait', gemalde:'painting' };

function tryGenerate(n, raw, lang) {
  const hasVerb = GEN_VERBS.some(v => n.includes(' ' + v + ' '));
  let noun = null; for (const k in NOUN_STYLE) if (n.includes(' ' + k + ' ')) { noun = k; break; }
  if (noun === 'fondo' && !hasVerb && lang !== 'es') noun = null;   // Italian "in fondo" (at the bottom) is not a background
  // request forms ("disegnami X" / "draw me X" / "dibújame X" / "dessine-moi X" / "zeichne mir X") don't need an image-noun
  // …and so do the plain DRAWING verbs ("disegna un gatto", "pinta una rosa", "dessine un chat"): what follows is the subject
  const reqForm = hasAny(n,'disegnami','disegnarmi','dipingimi','raffigurami','dibujame','pintame','hazme','dessine-moi','fais-moi',
    'disegna','dipingi','draw','paint','sketch','dibuja','pinta','dessine','peins','zeichne','male');
  if (!hasVerb && !noun) return null;
  if (!noun && !reqForm) return null;                       // a bare gen-verb without an object isn't a generate
  const style = noun ? NOUN_STYLE[noun] : 'image';
  // extract the subject prompt: text AFTER "noun di/of …", else after the verb, stripped of lead-in words.
  let p = norm(raw);
  const cut = (marker) => { const i = p.indexOf(' ' + marker + ' '); if (i >= 0) p = p.slice(i + marker.length + 2); };
  if (noun) { cut(noun); } else {
    let at = -1, len = 0;                                   // the request form itself, else the first gen verb
    for (const v of ['disegnami','disegnarmi','dipingimi','raffigurami','dibujame','pintame','hazme','dessine-moi','fais-moi', ...GEN_VERBS]) { const i = p.indexOf(' ' + v + ' '); if (i >= 0) { at = i; len = v.length; break; } }
    if (at >= 0) p = p.slice(at + len + 2);
  }
  p = ' ' + p.trim() + ' ';
  for (const lead of [' me ',' mir ',' di ',' of ',' del ',' della ',' dello ',' per ',' for ',' a ',' an ',' the ',' un ',' uno ',' una ',' che rappresenta ',' che raffigura ',' raffigurante ',' con ',
    ' de ',' para ',' el ',' la ',' d ',' du ',' une ',' le ',' pour ',' avec ',' von ',' fur ',' eines ',' einer ',' ein ',' eine ',' einen ',' mit ']) { if (p.startsWith(lead)) { p = ' ' + p.slice(lead.length); } }
  for (const art of ['un ','uno ','una ','il ','lo ','la ','i ','gli ','le ','a ','an ','the ','el ','une ','ein ','eine ','einen ','einer ','eines ']) { if (p.trim().startsWith(art)) { p = ' ' + p.trim().slice(art.length); } }
  p = p.trim();
  if (!p) p = (style === 'icon' ? ({ it:'un\'icona semplice', es:'un icono sencillo', fr:'une icône simple', de:'ein einfaches Symbol' }[lang] || 'a simple icon')
    : ({ it:'un\'immagine astratta', es:'una imagen abstracta', fr:'une image abstraite', de:'ein abstraktes Bild' }[lang] || 'an abstract image'));
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
