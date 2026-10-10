#!/usr/bin/env node
// Generate the Costellazioni art with Qwen-Image 2.1 (turbo, int8) through a local ComfyUI API
// (http://127.0.0.1:8188, started from C:\Users\niki0\Documents\ComfyUI). Reads prompts.json; every image
// is reproducible from its prompt + seed. Images whose entry has "refs" (cast ids) are made with the edit
// path: the cast portraits are passed as reference images so the faces stay the same across scenes.
// Raw PNGs land in tools/costellazioni-assets/raw/ (git-ignored); build-assets.mjs turns them into the
// shipped WebP set + manifest.
//   node tools/costellazioni-assets/gen-images.mjs [id,id,...|all] [--steps 20] [--force]
import { readFileSync, writeFileSync, existsSync, mkdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const RAW = join(here, 'raw');
const API = process.env.COMFY || 'http://127.0.0.1:8188';
const args = process.argv.slice(2);
const want = args[0] && !args[0].startsWith('--') ? args[0] : 'all';
const STEPS = args.includes('--steps') ? +args[args.indexOf('--steps') + 1] : 20;   // the user's call: 20 steps, quality
const FORCE = args.includes('--force');
const SAMPLER = args.includes('--sampler') ? args[args.indexOf('--sampler') + 1] : 'euler';
const SCHED = args.includes('--sched') ? args[args.indexOf('--sched') + 1] : 'simple';
const TAG = args.includes('--tag') ? `.${args[args.indexOf('--tag') + 1]}` : '';   // A/B tests: raw/<id>.<tag>.png
const spec = JSON.parse(readFileSync(join(here, 'prompts.json'), 'utf8'));
mkdirSync(RAW, { recursive: true });

// the base model: the user wants 20 sampling steps, and the turbo distillate over-sharpens past its native 8
const MODEL = process.argv.includes('--turbo') ? 'qwen_image_2.1_turbo_int8_convrot.safetensors' : 'qwen_image_2.1_int8_convrot.safetensors';
const TE = 'qwen3vl_8b_int8_convrot.safetensors';
const VAE = 'qwen_image_2.1_vae_bf16.safetensors';

let INFO = null;                                     // /object_info: input names of the nodes we use
async function nodeInputs(cls) {
  if (!INFO) INFO = await (await fetch(`${API}/object_info`)).json();
  const n = INFO[cls]; if (!n) throw new Error(`ComfyUI has no node ${cls} (update ComfyUI)`);
  return { ...(n.input.required || {}), ...(n.input.optional || {}) };
}
function promptText(img) {
  const style = (img.style || []).map((s) => spec.styles[s]).filter(Boolean).join(', ');
  return `${img.prompt}. ${style}`;
}

async function upload(file) {                         // a cast portrait as a ComfyUI input image
  const buf = readFileSync(file);
  const fd = new FormData();
  fd.append('image', new Blob([buf], { type: 'image/png' }), file.split(/[\\/]/).pop());
  fd.append('overwrite', 'true');
  const r = await fetch(`${API}/upload/image`, { method: 'POST', body: fd });
  if (!r.ok) throw new Error(`upload ${file}: ${r.status}`);
  return (await r.json()).name;
}

async function graph(img) {
  const te = await nodeInputs('TextEncodeQwenImage21');
  const cache = await nodeInputs('QwenImage21Cache');
  const g = {
    unet: { class_type: 'UNETLoader', inputs: { unet_name: MODEL, weight_dtype: 'default' } },
    clip: { class_type: 'CLIPLoader', inputs: { clip_name: TE, type: 'qwen_image', device: 'default' } },
    vae: { class_type: 'VAELoader', inputs: { vae_name: VAE } },
    lat: { class_type: 'EmptyLatentImage', inputs: { width: img.w, height: img.h, batch_size: 1 } },
  };
  // QwenImage21Cache wraps the model (template: widgets "auto", "default")
  const cin = { model: ['unet', 0] };
  const ck = Object.keys(cache).filter((k) => k !== 'model');
  if (ck[0]) cin[ck[0]] = 'auto';
  if (ck[1]) cin[ck[1]] = 'default';
  g.cache = { class_type: 'QwenImage21Cache', inputs: cin };
  // the text encoder: clip + prompt (+ reference images for the cast)
  const tin = { clip: ['clip', 0] };
  const keys = Object.keys(te);
  const promptKey = keys.find((k) => /^prompt$/.test(k)) || keys.find((k) => /prompt/.test(k) && !/neg/.test(k));
  const negKey = keys.find((k) => /neg/.test(k));
  tin[promptKey] = promptText(img);
  if (negKey) tin[negKey] = '';
  for (const k of keys) if (te[k][0] === 'VAE') tin[k] = ['vae', 0];
  for (const k of keys) if (te[k][0] === 'INT' && !(k in tin)) tin[k] = te[k][1] && te[k][1].default !== undefined ? te[k][1].default : 1024;
  // reference images: an autogrow input ("images" → images.image_1 … images.image_16 in the API prompt)
  if (img.refs && img.refs.length) {
    const grow = keys.find((k) => te[k][0] === 'COMFY_AUTOGROW_V3');
    const names = grow ? te[grow][1].template.names : [];
    for (let i = 0; i < img.refs.length && i < names.length; i++) {
      const name = await upload(join(RAW, `${img.refs[i]}.png`));
      g[`ref${i}`] = { class_type: 'LoadImage', inputs: { image: name } };
      tin[`${grow}.${names[i]}`] = [`ref${i}`, 0];
    }
  }
  g.enc = { class_type: 'TextEncodeQwenImage21', inputs: tin };
  g.ks = { class_type: 'KSampler', inputs: { model: ['cache', 0], positive: ['enc', 0], negative: ['enc', 1], latent_image: ['lat', 0],
    seed: img.seed, steps: STEPS, cfg: 1, sampler_name: SAMPLER, scheduler: SCHED, denoise: 1 } };
  g.dec = { class_type: 'VAEDecode', inputs: { samples: ['ks', 0], vae: ['vae', 0] } };
  g.save = { class_type: 'SaveImage', inputs: { images: ['dec', 0], filename_prefix: `stelle_${img.id}` } };
  return g;
}

async function run(img) {
  const out = join(RAW, `${img.id}${TAG}.png`);
  if (existsSync(out) && !FORCE) { console.log(`skip ${img.id} (exists)`); return; }
  const t0 = Date.now();
  const r = await fetch(`${API}/prompt`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ prompt: await graph(img) }) });
  const j = await r.json();
  if (!r.ok || !j.prompt_id) throw new Error(`queue ${img.id}: ${JSON.stringify(j).slice(0, 400)}`);
  for (;;) {                                          // poll the history until the image is there
    await new Promise((res) => setTimeout(res, 2000));
    const h = await (await fetch(`${API}/history/${j.prompt_id}`)).json();
    const e = h[j.prompt_id];
    if (!e) continue;
    if (e.status && e.status.status_str === 'error') throw new Error(`${img.id}: ${JSON.stringify(e.status.messages).slice(0, 600)}`);
    const o = Object.values(e.outputs || {}).find((x) => x.images && x.images.length);
    if (!o) continue;
    const f = o.images[0];
    const png = await (await fetch(`${API}/view?filename=${encodeURIComponent(f.filename)}&subfolder=${encodeURIComponent(f.subfolder)}&type=${f.type}`)).arrayBuffer();
    writeFileSync(out, Buffer.from(png));
    console.log(`ok   ${img.id}  ${img.w}x${img.h}  ${((Date.now() - t0) / 1000).toFixed(0)} s`);
    return;
  }
}

const list = want === 'all' ? spec.images : spec.images.filter((i) => want.split(',').includes(i.id));
// cast portraits first: scenes reference them
list.sort((a, b) => (a.kind === 'cast' ? 0 : 1) - (b.kind === 'cast' ? 0 : 1));
for (const img of list) {
  try { await run(img); } catch (e) { console.log(`FAIL ${e.message}`); }
}
