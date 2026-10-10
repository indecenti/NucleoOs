// paint-nlcommand.test.mjs — deterministic host gate for Paint's natural-language command parser
// (apps/paint/www/nlcommand.js). Proves: editing phrases map to the right op, generation requests are
// detected with the right style+prompt, and gibberish/unrelated text returns 'unknown' (never invents).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseCommand, buildGeneration } from '../../apps/paint/www/nlcommand.js';

const op = (s, lang) => parseCommand(s, lang).op;
const kind = (s, lang) => parseCommand(s, lang).kind;

test('editing phrases map to the correct op (IT)', () => {
  const cases = {
    'ruota a destra':'rotcw', 'gira a sinistra':'rotccw', "ruota l'immagine":'rotcw',
    'rifletti in orizzontale':'fliph', 'capovolgi verticalmente':'flipv',
    'rimuovi lo sfondo':'removeBg', 'togli lo sfondo del gatto':'removeBg', 'scontorna il soggetto':'removeBg',
    'scala di grigi':'fxGray', 'bianco e nero':'fxGray', 'inverti i colori':'fxInvert', 'applica il seppia':'fxSepia',
    "sfoca l'immagine":'fxBlur', 'aumenta la nitidezza':'fxSharp', 'mostra i contorni':'fxEdges',
    'rifila i bordi trasparenti':'trim', 'ritaglia alla selezione':'cropSel',
    'nuovo livello':'layerAdd', 'aggiungi un livello':'layerAdd', 'duplica il livello':'layerDup',
    'unisci sotto':'layerMerge', "appiattisci l'immagine":'layerFlatten', 'elimina il livello':'layerDel',
    'annulla':'undo', 'ripeti':'redo', 'salva':'save', 'cancella tutto':'clear',
    'mostra la griglia':'toggleGrid', 'nascondi i righelli':'toggleRulers', 'deseleziona':'deselect',
    'adatta alla finestra':'fitZoom',
  };
  for (const [phrase, want] of Object.entries(cases)) assert.equal(op(phrase, 'it'), want, `"${phrase}" → ${want}`);
});

test('editing phrases map to the correct op (EN)', () => {
  const cases = { 'rotate right':'rotcw', 'rotate left':'rotccw', 'flip horizontal':'fliph', 'remove the background':'removeBg',
    'grayscale':'fxGray', 'invert colors':'fxInvert', 'blur the image':'fxBlur', 'sharpen':'fxSharp',
    'new layer':'layerAdd', 'merge down':'layerMerge', 'flatten':'layerFlatten', 'undo':'undo', 'save':'save' };
  for (const [phrase, want] of Object.entries(cases)) assert.equal(op(phrase, 'en'), want, `"${phrase}" → ${want}`);
});

test('parametric edits: adjust / opacity / resize / zoom / fill', () => {
  let d = parseCommand('luminosità +20'); assert.equal(d.op,'adjust'); assert.equal(d.which,'brightness'); assert.equal(d.value,20); assert.equal(d.mode,'delta');
  d = parseCommand('aumenta la luminosità'); assert.equal(d.which,'brightness'); assert.equal(d.value,20);
  d = parseCommand('diminuisci il contrasto'); assert.equal(d.which,'contrast'); assert.equal(d.value,-20);
  d = parseCommand('porta la saturazione a 50'); assert.equal(d.which,'saturation'); assert.equal(d.value,50); assert.equal(d.mode,'set');
  d = parseCommand('opacità 40%'); assert.equal(d.op,'opacity'); assert.equal(d.value,40);
  d = parseCommand('ridimensiona a 512x512'); assert.equal(d.op,'resize'); assert.equal(d.w,512); assert.equal(d.h,512);
  d = parseCommand('zoom 200%'); assert.equal(d.op,'zoom'); assert.equal(d.value,200);
  d = parseCommand('ingrandisci'); assert.equal(d.op,'zoom'); assert.equal(d.dir,'in');
  d = parseCommand('riempi di rosso'); assert.equal(d.op,'fill'); assert.equal(d.color,'#ed1c24');
});

test('generation: image / icon / logo / drawing with extracted prompt + style', () => {
  let d = parseCommand("crea un'icona di un gatto"); assert.equal(d.kind,'generate'); assert.equal(d.style,'icon'); assert.match(d.prompt, /gatto/);
  d = parseCommand('genera un immagine di un tramonto sul mare'); assert.equal(d.kind,'generate'); assert.equal(d.style,'image'); assert.match(d.prompt,/tramonto sul mare/);
  d = parseCommand('disegnami un drago'); assert.equal(d.kind,'generate'); assert.match(d.prompt,/drago/);
  d = parseCommand('draw me a dragon','en'); assert.equal(d.kind,'generate'); assert.match(d.prompt,/dragon/);
  d = parseCommand('crea un logo per una caffetteria'); assert.equal(d.kind,'generate'); assert.equal(d.style,'logo'); assert.match(d.prompt,/caffetteria/);
  d = parseCommand('genera uno sfondo astratto'); assert.equal(d.kind,'generate'); assert.equal(d.style,'background'); assert.match(d.prompt,/astratto/);
  // icon build → flat style + auto background removal
  const b = buildGeneration({ style:'icon', prompt:'un gatto' }); assert.equal(b.removeBg, true); assert.match(b.prompt,/icon/);
});

test('disambiguation: "livello"/"sfondo" objects route correctly, NOT to generate', () => {
  assert.equal(parseCommand('crea un nuovo livello').op, 'layerAdd');
  assert.equal(parseCommand('rimuovi il livello attivo').op, 'layerDel');
  assert.equal(parseCommand('rimuovi lo sfondo').op, 'removeBg');
});

test('gibberish / unrelated text → unknown (never fabricates an action)', () => {
  for (const s of ['asdfgh qwerty', 'ciao come stai', 'che ore sono', '12345', 'blablabla', '']) {
    assert.equal(kind(s), 'unknown', `"${s}" must be unknown`);
  }
});

// Five languages (2026-10-10): es/fr/de phrases were all "unknown", and every non-English UI got Italian replies.
test('editing phrases map to the correct op (ES / FR / DE), replies in that language', () => {
  const cases = {
    es: { 'gira a la derecha':'rotcw', 'gira a la izquierda':'rotccw', 'quita el fondo':'removeBg', 'escala de grises':'fxGray', 'invierte los colores':'fxInvert',
      'desenfoca la imagen':'fxBlur', 'nueva capa':'layerAdd', 'duplica la capa':'layerDup', 'elimina la capa':'layerDel', 'deshacer':'undo', 'guarda':'save', 'voltea en vertical':'flipv' },
    fr: { 'tourne à droite':'rotcw', 'tourne à gauche':'rotccw', "supprime l'arrière-plan":'removeBg', 'noir et blanc':'fxGray', 'inverse les couleurs':'fxInvert',
      'ajoute un flou':'fxBlur', 'nouveau calque':'layerAdd', 'supprime le calque':'layerDel', 'annule':'undo', 'enregistre':'save', 'miroir horizontal':'fliph', 'aplatis l image':'layerFlatten' },
    de: { 'nach rechts drehen':'rotcw', 'nach links drehen':'rotccw', 'Hintergrund entfernen':'removeBg', 'Graustufen':'fxGray', 'Farben invertieren':'fxInvert',
      'weichzeichnen':'fxBlur', 'neue Ebene':'layerAdd', 'Ebene löschen':'layerDel', 'rückgängig':'undo', 'speichern':'save', 'vertikal spiegeln':'flipv', 'Raster':'toggleGrid' },
  };
  for (const [lang, m] of Object.entries(cases)) for (const [phrase, want] of Object.entries(m)) assert.equal(op(phrase, lang), want, `${lang}: "${phrase}" → ${want}`);
  assert.equal(parseCommand('gira a la derecha', 'es').reply, 'Giro a la derecha');
  assert.equal(parseCommand('nouveau calque', 'fr').reply, 'Nouveau calque');
  assert.equal(parseCommand('neue Ebene', 'de').reply, 'Neue Ebene');
  assert.match(parseCommand('xyz qqq', 'de').reply, /nicht verstanden/);
  let d = parseCommand('sube el brillo', 'es'); assert.equal(d.which, 'brightness'); assert.equal(d.value, 20); assert.match(d.reply, /^Brillo/);
  d = parseCommand('Helligkeit auf 50', 'de'); assert.equal(d.which, 'brightness'); assert.equal(d.mode, 'set'); assert.equal(d.value, 50);
  d = parseCommand('baisse le contraste', 'fr'); assert.equal(d.which, 'contrast'); assert.equal(d.value, -20);
  d = parseCommand('rellena de rojo', 'es'); assert.equal(d.op, 'fill'); assert.equal(d.color, '#ed1c24');
  d = parseCommand('fülle mit weiß', 'de'); assert.equal(d.op, 'fill'); assert.equal(d.color, '#ffffff');
});

test('generation in ES / FR / DE, and no cross-language false friends', () => {
  let d = parseCommand('crea un icono de un gato', 'es'); assert.equal(d.kind, 'generate'); assert.equal(d.style, 'icon'); assert.match(d.prompt, /gato/);
  d = parseCommand('dessine-moi un dragon', 'fr'); assert.equal(d.kind, 'generate'); assert.match(d.prompt, /^dragon/);
  d = parseCommand('zeichne mir einen Drachen', 'de'); assert.equal(d.kind, 'generate'); assert.match(d.prompt, /^drachen/);
  d = parseCommand('erstelle ein Logo für ein Café', 'de'); assert.equal(d.style, 'logo'); assert.match(d.prompt, /cafe/);
  d = parseCommand('pinta una rosa', 'es'); assert.equal(d.kind, 'generate', 'a drawing of a rose, not a pink fill');
  assert.notEqual(op('guarda come viene', 'it'), 'save', 'Italian "guarda" = look');
  assert.equal(kind('sposta il livello in fondo', 'it') === 'generate', false, 'Italian "in fondo" is not a background');
});
