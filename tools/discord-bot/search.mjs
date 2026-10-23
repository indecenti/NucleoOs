// Tiny BM25 over kb.json entries (title x3 + keywords x2 + answer). No dependencies, English + Italian words.
export const GUIDES = {
    start: 'What NucleoOS is', install: 'Install', webos: 'Web desktop', device: 'Using the Cardputer',
    anima: 'ANIMA assistant', update: 'Updates', troubleshooting: 'Troubleshooting',
};

const STOP = new Set(('a an and are can do does for from how i in is it me my of on or the to what where which with you your '
    + 'il lo la le gli un una e di da in su per con come cosa che non mi si posso devo').split(' '));

export function tokens(s) {
    return String(s).toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '')
        .replace(/[^a-z0-9.+#-]+/g, ' ').split(' ')
        .map(w => w.replace(/^[.-]+|[.-]+$/g, ''))
        .filter(w => w.length > 1 && !STOP.has(w))
        .map(w => w.length > 4 ? w.replace(/(ing|ed|es|s)$/, '') : w);    // crude stemming: install/installing
}

const cache = new WeakMap();
function index(entries) {
    if (cache.has(entries)) return cache.get(entries);
    const docs = entries.map(e => {
        const tf = new Map();
        const add = (text, w) => { for (const t of tokens(text)) tf.set(t, (tf.get(t) || 0) + w); };
        add(e.title, 3); add(e.keywords, 2);
        const key = new Set(tf.keys());              // words the entry is ABOUT (title + keywords)
        add(e.answer, 1);
        let len = 0; for (const v of tf.values()) len += v;
        return { e, tf, len, key };
    });
    const avg = docs.reduce((a, d) => a + d.len, 0) / docs.length;
    const df = new Map();
    for (const d of docs) for (const t of d.tf.keys()) df.set(t, (df.get(t) || 0) + 1);
    const ix = { docs, avg, df, n: docs.length };
    cache.set(entries, ix);
    return ix;
}

// Returns up to 3 entries, best first; [] when nothing matches well enough (better silence than a wrong answer).
export function search(entries, query, minScore = 2.2) {
    const { docs, avg, df, n } = index(entries);
    const q = [...new Set(tokens(query))];
    if (!q.length) return [];
    const k1 = 1.2, b = 0.75;
    const scored = docs.filter(d => q.some(t => d.key.has(t))).map(d => {   // a passing mention in an answer is not a match
        let s = 0;
        for (const t of q) {
            const f = d.tf.get(t); if (!f) continue;
            const idf = Math.log(1 + (n - df.get(t) + 0.5) / (df.get(t) + 0.5));
            s += idf * (f * (k1 + 1)) / (f + k1 * (1 - b + b * d.len / avg));
        }
        return { e: d.e, s };
    }).filter(x => x.s >= minScore).sort((a, b) => b.s - a.s);
    if (!scored.length) return [];
    return scored.filter(x => x.s >= scored[0].s * 0.6).slice(0, 3).map(x => x.e);
}

export const byGuide = (entries, topic) => entries.filter(e => e.guide === topic);
