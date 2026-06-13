// paint-prompt-corpus.mjs — a shared set of REAL image-generation prompts (IT + EN, mixed styles) that the
// Atelier pipeline tests run end to end. Used by paint-pipeline / paint-online-image / paint-prompt-enhance /
// paint-diffusion-engine so "it works on real prompts" is proven by the same corpus everywhere, not toy strings.

export const PROMPTS = [
  { text: 'un gatto rosso seduto su un divano, luce calda',                 style: 'image',        lang: 'it' },
  { text: 'a serene mountain lake at sunrise, drifting mist',               style: 'background',   lang: 'en' },
  { text: "crea un'icona di una bussola",                                   style: 'icon',         lang: 'it' },
  { text: "logo for a coffee shop named 'Nucleo'",                          style: 'logo',         lang: 'en' },
  { text: 'ritratto di una donna anziana che sorride',                      style: 'portrait',     lang: 'it' },
  { text: 'a watercolor painting of the Venice canals at dusk',            style: 'painting',     lang: 'en' },
  { text: 'un drago che vola su una città cyberpunk al neon',              style: 'image',        lang: 'it' },
  { text: 'flat illustration of a rocket launching into space',           style: 'illustration', lang: 'en' },
  { text: 'foto realistica di un piatto di spaghetti al pomodoro',         style: 'photo',        lang: 'it' },
  { text: 'disegno di un robot amichevole che saluta',                     style: 'drawing',      lang: 'it' },
  { text: 'a minimalist app icon of a paint brush, single colour',         style: 'icon',         lang: 'en' },
  { text: 'paesaggio autunnale con foglie che cadono nel bosco',          style: 'background',   lang: 'it' },
];

// A negative-prompt fixture (the image API has no native negative field → folded into the prompt).
export const NEGATIVES = ['sfocato, deforme, testo, filigrana', 'lowres, watermark, extra fingers'];
