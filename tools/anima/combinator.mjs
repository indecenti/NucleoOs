// ANIMA neuro-symbolic combinators — COMPUTE answers by composing multiple facts, deterministically.
// The leap past recall/deduction: "chi è nato prima, Dante o Einstein?", "quanti anni tra la loro
// nascita?", "Einstein era europeo?", "Dante e Einstein erano connazionali?" — answers that exist
// NOWHERE until you combine ≥2 facts. The VSA/KGE tiers fetch the facts (offline if known, else learn
// online once); these combinators do the typed composition (compare / subtract / geo-contain / equal),
// returning a result with EVIDENTIAL confidence (the weakest source bounds it) and provenance. No LLM,
// no generation → cannot hallucinate: if a needed fact is missing, it refuses.
//
// Pure logic over a `getFact(entity, rel, lang)` oracle (injected, so it is testable with a seeded fact
// base AND wired to the real Wikidata fetcher). getFact returns {year}|{value,label}|null.

const norm = (s) => String(s).toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/['’]/g, ' ').replace(/\s+/g, ' ').trim();
const slug = (s) => norm(s).replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '');
const cap = (s) => String(s).replace(/\b([a-zàèéìòù])/g, (c) => c.toUpperCase());
const cleanEnt = (s) => norm(s).replace(/[?.!]+$/, '').replace(/^(?:il |lo |la |l |i |gli |le |un |uno |una )/, '').trim();
const DEMONYM = { europeo: 'europa', europea: 'europa', asiatico: 'asia', asiatica: 'asia', americano: 'america', americana: 'america', africano: 'africa', africana: 'africa', oceaniano: 'oceania', oceaniana: 'oceania' };

// Parse a composition question into {op, ...}. Tight patterns; anything else → null (falls through).
export function parseQuery(q) {
  const s = norm(q).replace(/[?.!]+$/, '');
  let m;
  if ((m = s.match(/^chi (?:e|era) (?:nat[oa] prima|piu (?:vecchi[oa]|anzian[oa]))(?:,)?\s+(.+?)\s+o\s+(.+)$/))) return { op: 'older', a: cleanEnt(m[1]), b: cleanEnt(m[2]) };
  if ((m = s.match(/^chi (?:e|era) (?:piu (?:giovane|recente)|nat[oa] dopo)(?:,)?\s+(.+?)\s+o\s+(.+)$/))) return { op: 'younger', a: cleanEnt(m[1]), b: cleanEnt(m[2]) };
  if ((m = s.match(/quanti anni\s+(?:passano\s+|ci sono\s+)?(?:tra|fra|separano|dividono|intercorrono)\s+(?:la nascita di\s+)?(.+?)\s+ed?\s+(?:quella di\s+|la nascita di\s+)?(.+)$/))) return { op: 'diffyears', a: cleanEnt(m[1]), b: cleanEnt(m[2]) };
  if ((m = s.match(/^(.+?)\s+(?:era|e)\s+(?:un |una )?(europe[oa]|asiatic[oa]|american[oa]|african[oa]|oceanian[oa])\b/))) return { op: 'continent', a: cleanEnt(m[1]), cont: DEMONYM[m[2]] };
  if ((m = s.match(/^(.+?)\s+e\s+(.+?)\s+(?:erano|sono)\s+(?:connazionali|dello stesso paese|della stessa nazione)/))) return { op: 'samecountry', a: cleanEnt(m[1]), b: cleanEnt(m[2]) };
  return null;
}

// Compute the answer. Returns {reply, intent:'combinator', op, confidence, provenance} or null.
export async function answer(q, getFact, lang = 'it') {
  const p = parseQuery(q); if (!p) return null;
  const conf = (parts) => Math.round(Math.min(...parts.map(x => x?.conf ?? 0.9)) * 100);
  const wrap = (reply, parts) => ({ query: q, tier: 'fact', action: 'answer', intent: 'combinator', op: p.op, state: 'idle', reply, confidence: conf(parts), provenance: parts.map(x => x?.src).filter(Boolean) });

  if (p.op === 'older' || p.op === 'younger') {
    const A = await getFact(p.a, 'born', lang), B = await getFact(p.b, 'born', lang);
    if (!A?.year || !B?.year) return null;
    const earlier = A.year <= B.year ? { n: p.a, y: A.year } : { n: p.b, y: B.year };
    const later = A.year <= B.year ? { n: p.b, y: B.year } : { n: p.a, y: A.year };
    const pick = p.op === 'older' ? earlier : later;
    return wrap(`${cap(pick.n)} (${earlier.y === pick.y ? earlier.y : later.y}). ${cap(earlier.n)} è nato nel ${earlier.y}, ${cap(later.n)} nel ${later.y}.`, [A, B]);
  }
  if (p.op === 'diffyears') {
    const A = await getFact(p.a, 'born', lang), B = await getFact(p.b, 'born', lang);
    if (!A?.year || !B?.year) return null;
    const d = Math.abs(A.year - B.year);
    return wrap(`Tra la nascita di ${cap(p.a)} (${A.year}) e quella di ${cap(p.b)} (${B.year}) ci sono ${d} anni.`, [A, B]);
  }
  if (p.op === 'continent') {
    const nat = await getFact(p.a, 'nationality', lang); if (!nat?.value) return null;
    const cont = await getFact(nat.value, 'continent', lang); if (!cont?.value) return null;
    const yes = slug(cont.value) === p.cont || slug(cont.value).includes(p.cont) || p.cont.includes(slug(cont.value));
    return wrap(yes
      ? `Sì: ${cap(p.a)} era di ${nat.value}, che è in ${cont.value}.`
      : `No: ${cap(p.a)} era di ${nat.value}, che è in ${cont.value}, non in ${cap(p.cont)}.`, [nat, cont]);
  }
  if (p.op === 'samecountry') {
    const A = await getFact(p.a, 'nationality', lang), B = await getFact(p.b, 'nationality', lang);
    if (!A?.value || !B?.value) return null;
    const same = slug(A.value) === slug(B.value);
    return wrap(same
      ? `Sì: ${cap(p.a)} e ${cap(p.b)} erano entrambi di ${A.value}.`
      : `No: ${cap(p.a)} era di ${A.value}, ${cap(p.b)} di ${B.value}.`, [A, B]);
  }
  return null;
}
