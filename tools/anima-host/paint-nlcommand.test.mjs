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
