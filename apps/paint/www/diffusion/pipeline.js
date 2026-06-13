// pipeline.js — the Atelier image pipeline orchestrator. Pure & DOM-free (engines + enhancer INJECTED) →
// host-tested with real prompts. This is the single place that makes the whole flow LLM-driven:
//
//     idea ──(optional)──▶ LLM enhancer ──▶ professional prompt ──▶ generative engine ──▶ image(s)
//
// The engine is whatever the user selected (online cloud model, local in-browser diffusion model, or the
// explicit procedural preview) — resolved lazily via `resolveEngine(providerId)` so the heavy work only
// happens for the chosen path. Variants are produced by re-seeding; for the local model that is
// deterministic (same seed ⇒ same bytes), for the online model the seed is best-effort metadata.
//
// makeImagePipeline({ resolveEngine, enhancer })
//   resolveEngine(id) -> Promise<engine>     engine = { generate({prompt,negativePrompt,seed,sketch,
//                                            controlScale}, onStep) -> {image,meta}, brand?, isOnline? }
//   enhancer (optional) = { available, brand, enhance(raw,{style,negative,lang}) -> {prompt,negative,source} }
//
//   .run(opts) -> { results:[{image,seed,meta}], errors:[], enhanced|null, provider, brand, online, prompt, negative }

export function makeImagePipeline({ resolveEngine, enhancer } = {}) {
  if (typeof resolveEngine !== 'function') throw new Error('resolveEngine must be injected');

  return {
    async run({
      provider = 'preview', prompt = '', negativePrompt = '', style = 'image',
      enhance = false, n = 1, seed = 42, sketch = null, controlScale = 1, lang = 'it',
      onStage = () => {},
    } = {}) {
      const original = String(prompt || '').trim();
      const nVar = Math.max(1, Math.min(8, n | 0 || 1));

      // 1) LLM enhancement (optional, additive — never blocks generation)
      let finalPrompt = original, finalNeg = String(negativePrompt || '').trim(), enhanced = null;
      if (enhance && enhancer && enhancer.available && original) {
        onStage({ phase: 'enhance', state: 'active', brand: enhancer.brand });
        const e = await enhancer.enhance(original, { style, negative: finalNeg, lang });
        finalPrompt = e.prompt || original;
        if (e.negative) finalNeg = e.negative;
        enhanced = { from: original, to: finalPrompt, source: e.source, brand: e.brand };
        onStage({ phase: 'enhance', state: 'done', brand: enhancer.brand, source: e.source, prompt: finalPrompt });
      }

      // 2) resolve the chosen engine (lazy: only the selected path pays the cost)
      onStage({ phase: 'engine', state: 'active', provider });
      const engine = await resolveEngine(provider);
      if (!engine || typeof engine.generate !== 'function') throw new Error('motore non disponibile: ' + provider);
      const brand = engine.brand || provider;
      const online = !!engine.isOnline;
      onStage({ phase: 'engine', state: 'done', provider, brand });

      // 3) generate variants (best-effort: collect successes, surface per-variant errors; throw only if none)
      const results = [], errors = [];
      const seed0 = (seed >>> 0);
      for (let i = 0; i < nVar; i++) {
        const s = (seed0 + i) >>> 0;
        onStage({ phase: 'generate', state: 'active', index: i, total: nVar, seed: s });
        try {
          const out = await engine.generate(
            { prompt: finalPrompt, negativePrompt: finalNeg, seed: s, sketch, controlScale, n: 1 },
            (st) => onStage({ phase: 'step', index: i, step: st && st.step }),
          );
          results.push({ image: out.image, seed: s, meta: out.meta || {} });
          onStage({ phase: 'generate', state: 'done', index: i, total: nVar, seed: s });
        } catch (err) {
          errors.push({ index: i, seed: s, error: (err && err.message) || String(err), status: err && err.status });
          onStage({ phase: 'generate', state: 'error', index: i, total: nVar, seed: s, error: (err && err.message) || String(err) });
        }
      }
      if (!results.length) {
        const e = new Error(errors.length ? errors[errors.length - 1].error : 'generazione non riuscita');
        if (errors.length) e.status = errors[errors.length - 1].status;
        throw e;
      }
      return { results, errors, enhanced, provider, brand, online, prompt: finalPrompt, negative: finalNeg };
    },
  };
}
