/* SPDX-License-Identifier: AGPL-3.0-or-later */
// byte-source.js — accesso a blocchi a un file remoto (/api/fs/read) o locale (File/Blob).
//
// Il file è visto come una sequenza di blocchi da BLOCK byte. Due modalità di rete:
//  • "range"  — il server risponde 206 a `Range: bytes=a-b`: ogni blocco è una richiesta media
//               (≤1 in volo per il prefetch, 2 per le richieste urgenti: il server HTTP della board
//               è a task singolo e le richieste del Service Worker passano da un semaforo a 2).
//  • "stream" — il server ignora Range e risponde 200 con tutto il file (firmware attuale): si
//               legge il corpo in sequenza, i blocchi arrivano in ordine; tornare su un blocco già
//               scartato = ripartire da capo scartando i byte. Con file piccoli si tiene tutto.
// La modalità si scopre alla prima richiesta (Range 0..BLOCK-1 → 206 oppure 200).

// 256 KB: la prima immagine arriva prima (la board serve ~0,1-2 MB/s) e le richieste restano medie.
export const BLOCK = 256 * 1024;

const CACHE_MAX = 160 * 1024 * 1024;   // tetto RAM dei blocchi in modalità range (LRU)
const STREAM_KEEP = 256 * 1024 * 1024; // in modalità stream sotto questa taglia si tiene tutto
const STREAM_AHEAD = 192;              // blocchi massimi scaricati oltre l'ultimo richiesto (48 MB)
const STREAM_BACK = 48;                // blocchi tenuti dietro la posizione di lettura (12 MB, file grandi)

export class SourceError extends Error {
  constructor(key, detail) { super(key + (detail ? ': ' + detail : '')); this.key = key; this.detail = detail || ''; }
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

class BaseSource extends EventTarget {
  constructor() {
    super();
    this.size = null;          // byte totali (null = ancora ignoti)
    this.ranged = true;
    this.blocks = new Map();   // indice -> Uint8Array (BLOCK byte, l'ultimo può essere più corto)
    this.cachedBytes = 0;
    this.useTick = 0;
    this.lastUse = new Map();
    this.waiters = new Map();  // indice -> [{resolve, reject}]
    this.closed = false;
    this.stats = { bytes: 0, requests: 0, retries: 0, mode: 'range' };
  }
  get blockCount() { return this.size == null ? Infinity : Math.ceil(this.size / BLOCK); }
  peek(i) {
    const b = this.blocks.get(i);
    if (b) this.lastUse.set(i, ++this.useTick);
    return b;
  }
  // true se [off, off+len) è tutto in cache (o oltre la fine del file)
  has(off, len) {
    const end = this.size != null ? Math.min(off + len, this.size) : off + len;
    for (let i = Math.floor(off / BLOCK); i * BLOCK < end; i++) if (!this.blocks.has(i)) return false;
    return true;
  }
  // Copia sincrona di [off, off+len) se è in cache, altrimenti null. Evita la copia quando il
  // tratto sta in un solo blocco.
  readSync(off, len) {
    if (this.size != null) len = Math.max(0, Math.min(len, this.size - off));
    if (len <= 0) return new Uint8Array(0);
    const b0 = Math.floor(off / BLOCK), b1 = Math.floor((off + len - 1) / BLOCK);
    if (b0 === b1) {
      const b = this.peek(b0); if (!b) return null;
      const s = off - b0 * BLOCK;
      if (s + len > b.length) return null;
      return b.subarray(s, s + len);
    }
    const out = new Uint8Array(len);
    let o = 0;
    for (let i = b0; i <= b1; i++) {
      const b = this.peek(i); if (!b) return null;
      const s = i === b0 ? off - b0 * BLOCK : 0;
      const e = Math.min(b.length, off + len - i * BLOCK);
      out.set(b.subarray(s, e), o); o += e - s;
    }
    return o === len ? out : out.subarray(0, o);
  }
  async read(off, len, urgent = true) {
    if (this.size != null) len = Math.max(0, Math.min(len, this.size - off));
    if (len <= 0) return new Uint8Array(0);
    const jobs = [];
    for (let i = Math.floor(off / BLOCK); i * BLOCK < off + len; i++) jobs.push(this.getBlock(i, urgent));
    await Promise.all(jobs);
    const r = this.readSync(off, len);
    if (r) return r;
    // un blocco è stato scartato nel frattempo (cache piena): riprova una volta
    await Promise.all(jobs.map((_, k) => this.getBlock(Math.floor(off / BLOCK) + k, urgent)));
    return this.readSync(off, len) || new Uint8Array(0);
  }
  _put(i, data) {
    if (!this.blocks.has(i)) this.cachedBytes += data.length;
    else this.cachedBytes += data.length - this.blocks.get(i).length;
    this.blocks.set(i, data);
    this.lastUse.set(i, ++this.useTick);
    const w = this.waiters.get(i);
    if (w) { this.waiters.delete(i); w.forEach((x) => x.resolve(data)); }
    this.dispatchEvent(new Event('progress'));
  }
  _drop(i) {
    const b = this.blocks.get(i);
    if (!b) return;
    this.cachedBytes -= b.length;
    this.blocks.delete(i); this.lastUse.delete(i);
  }
  _wait(i) {
    return new Promise((resolve, reject) => {
      if (!this.waiters.has(i)) this.waiters.set(i, []);
      this.waiters.get(i).push({ resolve, reject });
    });
  }
  _failWaiters(err) {
    for (const [, arr] of this.waiters) arr.forEach((x) => x.reject(err));
    this.waiters.clear();
  }
  // Intervalli di byte in cache, fusi: [[inizio, fine), ...]
  cachedRanges() {
    const idx = [...this.blocks.keys()].sort((a, b) => a - b);
    const out = [];
    for (const i of idx) {
      const s = i * BLOCK, e = s + this.blocks.get(i).length;
      if (out.length && out[out.length - 1][1] === s) out[out.length - 1][1] = e;
      else out.push([s, e]);
    }
    return out;
  }
  // Tratto piccolo fuori dalla cache a blocchi (indici, coda del file): una sola richiesta Range
  // di esattamente quei byte invece di un blocco intero. Senza Range si ripiega su read().
  fetchRange(off, len) { return this.read(off, len, true); }
  prefetch() {}
  cancelPrefetch() {}
  close() { this.closed = true; this._failWaiters(new SourceError('aborted')); this.blocks.clear(); this.cachedBytes = 0; }
}

// ------------------------------------------------------------------------------------------------
// File locale (drag&drop / "Apri file locale"): accesso casuale gratuito via Blob.slice.

export class FileSource extends BaseSource {
  constructor(file) { super(); this.file = file; this.size = file.size; this.stats.mode = 'local'; this.pending = new Map(); }
  async open() { if (!this.size) throw new SourceError('errEmpty'); return this; }
  getBlock(i) {
    const hit = this.peek(i);
    if (hit) return Promise.resolve(hit);
    if (i * BLOCK >= this.size) return Promise.resolve(new Uint8Array(0));
    if (this.pending.has(i)) return this.pending.get(i);
    const p = this.file.slice(i * BLOCK, Math.min(this.size, (i + 1) * BLOCK)).arrayBuffer().then((ab) => {
      this.pending.delete(i);
      const u = new Uint8Array(ab);
      this.stats.bytes += u.length; this.stats.requests++;
      this._put(i, u); this._evict(i);
      return u;
    }, (e) => { this.pending.delete(i); throw new SourceError('errRead', e && e.message); });
    this.pending.set(i, p);
    return p;
  }
  prefetch(from, to) { for (let i = from; i <= to && i * BLOCK < this.size; i++) if (!this.blocks.has(i)) this.getBlock(i).catch(() => {}); }
  _evict(keep) {
    if (this.cachedBytes <= CACHE_MAX) return;
    const order = [...this.lastUse.entries()].sort((a, b) => a[1] - b[1]);
    for (const [i] of order) { if (this.cachedBytes <= CACHE_MAX * 0.8) break; if (i !== keep && i !== 0) this._drop(i); }
  }
}

// ------------------------------------------------------------------------------------------------
// File sulla MicroSD della board via /api/fs/read (Range se supportato, altrimenti stream).

export class HttpSource extends BaseSource {
  constructor(url, sizeHint) {
    super();
    this.url = url;
    this.sizeHint = sizeHint || null;
    this.queue = [];               // [{i, urgent}] blocchi da scaricare (modalità range)
    this.inflight = new Map();     // i -> {ctrl, urgent}
    this.streamCtl = null;         // AbortController del corpo in lettura (modalità stream)
    this.streamPos = 0;            // byte ricevuti dal flusso corrente
    this.streamGen = 0;
    this.readHead = 0;             // ultimo blocco richiesto dal player (per la contropressione)
    this.resumeStream = null;
    this.eof = false;
  }

  async open() {
    // Dimensione dall'elenco della cartella PRIMA di aprire il file: senza Range la board risponde
    // chunked senza Content-Length e, essendo a task singolo, non servirebbe altre richieste finché
    // il corpo non è stato mandato tutto.
    if (this.sizeHint == null) this.sizeHint = await this._sizeFromListing();
    let res, ctl;
    for (let attempt = 0; ; attempt++) {
      try {
        ctl = new AbortController();
        this.streamCtl = ctl;
        res = await fetch(this.url, { headers: { Range: `bytes=0-${BLOCK - 1}` }, cache: 'no-store', signal: ctl.signal });
        if ((res.status === 503 || res.status === 504) && attempt < 4) { this.stats.retries++; await sleep(400 * (attempt + 1)); continue; }
        break;
      } catch (e) {
        if (this.closed) throw new SourceError('aborted');
        if (attempt >= 3) throw new SourceError('errNetwork', e && e.message);
        this.stats.retries++; await sleep(500 * (attempt + 1));
      }
    }
    this.stats.requests++;
    if (res.status === 404) throw new SourceError('errNotFound');
    if (res.status === 416) throw new SourceError('errEmpty');
    if (res.status === 206) {
      this.ranged = true; this.stats.mode = 'range';
      const cr = res.headers.get('Content-Range') || '';
      const m = /\/\s*(\d+)\s*$/.exec(cr);
      if (m) this.size = parseInt(m[1], 10);
      const first = /bytes\s+(\d+)-/.exec(cr);
      const start = first ? parseInt(first[1], 10) : 0;
      const u = new Uint8Array(await res.arrayBuffer());
      this.stats.bytes += u.length;
      if (this.size == null) this.size = this.sizeHint || null;
      this._storeRange(start, u);
      if (this.size != null && this.size === 0) throw new SourceError('errEmpty');
      return this;
    }
    if (res.ok) {
      // Niente Range (firmware attuale): 200 con tutto il file. Si continua a leggere questo corpo.
      this.ranged = false; this.stats.mode = 'stream';
      const cl = parseInt(res.headers.get('Content-Length') || '', 10);
      this.size = Number.isFinite(cl) && cl >= 0 ? cl : (this.sizeHint || null);
      this._pump(res, ++this.streamGen, ctl);
      await this.getBlock(0);   // almeno l'inizio del file prima di dichiararlo aperto
      if (this.size === 0 || (this.eof && this.streamPos === 0)) throw new SourceError('errEmpty');
      return this;
    }
    throw new SourceError('errNetwork', 'HTTP ' + res.status);
  }

  // Dimensione del file dall'elenco della cartella (/api/fs/list), che la board fornisce sempre.
  async _sizeFromListing() {
    try {
      const u = new URL(this.url, location.href);
      const path = u.searchParams.get('path');
      if (!path || !u.pathname.endsWith('/api/fs/read')) return null;
      const dir = path.substring(0, path.lastIndexOf('/')) || '/';
      const name = path.substring(path.lastIndexOf('/') + 1);
      const r = await fetch('/api/fs/list?path=' + encodeURIComponent(dir), { cache: 'no-store' });
      if (!r.ok) return null;
      const e = ((await r.json()).entries || []).find((x) => x.name === name);
      return e && e.size > 0 ? e.size : null;
    } catch { return null; }
  }

  // Salva un tratto arbitrario ricevuto (es. risposta 206 più lunga del previsto).
  _storeRange(start, u) {
    let off = 0;
    while (off < u.length) {
      const pos = start + off;
      const i = Math.floor(pos / BLOCK);
      const bs = i * BLOCK;
      const want = this.size != null ? Math.min(BLOCK, this.size - bs) : BLOCK;
      if (pos === bs && u.length - off >= want) { this._put(i, u.slice(off, off + want)); off += want; }
      else if (pos === bs && this.size != null && start + u.length >= this.size) { this._put(i, u.slice(off)); off = u.length; }
      else break;   // blocco parziale: lo riscaricheremo intero
    }
    this._evict();
  }

  getBlock(i, urgent = true) {
    if (this.closed) return Promise.reject(new SourceError('aborted'));
    if (urgent) this.readHead = i;
    const hit = this.peek(i);
    if (hit) return Promise.resolve(hit);
    if (this.size != null && i * BLOCK >= this.size) return Promise.resolve(new Uint8Array(0));
    const p = this._wait(i);
    if (this.ranged) this._enqueue(i, urgent);
    else this._streamNeed(i);
    return p;
  }

  // Suggerimento: i blocchi [from, to] serviranno presto.
  prefetch(from, to) {
    if (this.closed) return;
    const last = this.size != null ? Math.min(to, this.blockCount - 1) : to;
    if (!this.ranged) { this._kickStream(); return; }
    for (let i = from; i <= last; i++) if (!this.blocks.has(i)) this._enqueue(i, false);
  }

  async fetchRange(off, len) {
    if (this.size != null) len = Math.max(0, Math.min(len, this.size - off));
    if (len <= 0) return new Uint8Array(0);
    if (!this.ranged || this.has(off, len)) return this.read(off, len, true);
    for (let attempt = 0; attempt < 4 && !this.closed; attempt++) {
      try {
        const res = await fetch(this.url, { headers: { Range: `bytes=${off}-${off + len - 1}` }, cache: 'no-store' });
        this.stats.requests++;
        if (res.status === 206) {
          const u = new Uint8Array(await res.arrayBuffer());
          this.stats.bytes += u.length;
          return u.length > len ? u.subarray(0, len) : u;
        }
        if (res.status === 200) { try { res.body && res.body.cancel(); } catch {} return this.read(off, len, true); }
        if (res.status === 404) throw new SourceError('errNotFound');
      } catch (e) {
        if (e instanceof SourceError) throw e;
      }
      this.stats.retries++;
      await sleep(300 * (attempt + 1));
    }
    throw new SourceError('errNetwork');
  }

  // Posizione di lettura corrente del player (contropressione e finestra in modalità stream).
  setPosition(off) {
    const i = Math.floor(off / BLOCK);
    if (i !== this.readHead) { this.readHead = i; if (!this.ranged) this._kickStream(); }
  }

  // Dopo un seek: dimentica i prefetch non ancora partiti e interrompe quelli lontani.
  cancelPrefetch(keepFrom = -1, keepTo = -1) {
    this.queue = this.queue.filter((q) => q.urgent || (q.i >= keepFrom && q.i <= keepTo));
    for (const [i, f] of this.inflight) {
      if (!f.urgent && !(i >= keepFrom && i <= keepTo) && !this.waiters.has(i)) f.ctrl.abort();
    }
    if (!this.ranged) this.readHead = Math.max(0, keepFrom);
  }

  // ---- modalità range
  _enqueue(i, urgent) {
    if (this.inflight.has(i)) { if (urgent) this.inflight.get(i).urgent = true; return; }
    const q = this.queue.find((x) => x.i === i);
    if (q) { if (urgent && !q.urgent) { q.urgent = true; } }
    else this.queue.push({ i, urgent });
    // urgenti prima, poi in ordine di indice (lettura sequenziale)
    this.queue.sort((a, b) => (b.urgent - a.urgent) || (a.i - b.i));
    this._drain();
  }
  _drain() {
    while (this.queue.length) {
      const head = this.queue[0];
      const limit = head.urgent ? 2 : 1;
      if (this.inflight.size >= limit) return;
      this.queue.shift();
      if (this.blocks.has(head.i)) continue;
      this._fetchBlock(head.i, head.urgent);
    }
  }
  async _fetchBlock(i, urgent) {
    const ctrl = new AbortController();
    const rec = { ctrl, urgent };
    this.inflight.set(i, rec);
    const start = i * BLOCK;
    const end = (this.size != null ? Math.min(this.size, start + BLOCK) : start + BLOCK) - 1;
    let err = null;
    for (let attempt = 0; attempt < 5 && !this.closed; attempt++) {
      try {
        const res = await fetch(this.url, { headers: { Range: `bytes=${start}-${end}` }, cache: 'no-store', signal: ctrl.signal });
        this.stats.requests++;
        if (res.status === 206) {
          const u = new Uint8Array(await res.arrayBuffer());
          this.stats.bytes += u.length;
          const cr = /bytes\s+(\d+)-(\d+)\/(\d+)/.exec(res.headers.get('Content-Range') || '');
          const got = cr ? parseInt(cr[1], 10) : start;
          if (cr && this.size == null) this.size = parseInt(cr[3], 10);
          this.inflight.delete(i);
          if (got === start) this._put(i, u.length > end - start + 1 ? u.slice(0, end - start + 1) : u);
          else this._storeRange(got, u);
          this._evict(i);
          this._drain();
          return;
        }
        if (res.status === 200) {
          // Il server ha smesso di onorare Range: si passa allo stream dall'inizio.
          this.inflight.delete(i);
          this._switchToStream(res);
          return;
        }
        if (res.status === 404) { err = new SourceError('errNotFound'); break; }
        if (res.status === 416) { this.inflight.delete(i); this._put(i, new Uint8Array(0)); this._drain(); return; }
        err = new SourceError('errNetwork', 'HTTP ' + res.status);
      } catch (e) {
        if (ctrl.signal.aborted) { this.inflight.delete(i); this._drain(); return; }   // annullato da un seek
        err = new SourceError('errNetwork', e && e.message);
      }
      this.stats.retries++;
      await sleep(300 * (attempt + 1) * (attempt + 1));
    }
    this.inflight.delete(i);
    const w = this.waiters.get(i);
    if (w && err) { this.waiters.delete(i); w.forEach((x) => x.reject(err)); }
    this._drain();
  }
  _evict(keep = -1) {
    if (this.cachedBytes <= CACHE_MAX) return;
    const order = [...this.lastUse.entries()].sort((a, b) => a[1] - b[1]);
    for (const [i] of order) {
      if (this.cachedBytes <= CACHE_MAX * 0.8) break;
      if (i !== keep && i !== 0 && !this.waiters.has(i)) this._drop(i);
    }
  }

  // ---- modalità stream (server senza Range)
  _switchToStream(res) {
    this.ranged = false; this.stats.mode = 'stream';
    for (const [, f] of this.inflight) f.ctrl.abort();
    this.inflight.clear(); this.queue = [];
    this._pump(res, ++this.streamGen);
  }
  _streamNeed(i) {
    const cur = Math.floor(this.streamPos / BLOCK);
    if (i < cur && !this.blocks.has(i)) this._restartStream();   // blocco già scartato: da capo
    else this._kickStream();
  }
  _kickStream() { if (this.resumeStream) { const r = this.resumeStream; this.resumeStream = null; r(); } }
  async _restartStream() {
    if (this.restarting) return;
    this.restarting = true;
    try {
      if (this.streamCtl) this.streamCtl.abort();
      const gen = ++this.streamGen;
      for (let attempt = 0; attempt < 5 && !this.closed; attempt++) {
        try {
          const ctl = new AbortController();
          const res = await fetch(this.url, { cache: 'no-store', signal: ctl.signal });
          this.stats.requests++;
          if (!res.ok) throw new Error('HTTP ' + res.status);
          if (gen !== this.streamGen) { ctl.abort(); return; }
          this._pump(res, gen, ctl);
          return;
        } catch (e) { this.stats.retries++; await sleep(500 * (attempt + 1)); }
      }
      this._failWaiters(new SourceError('errNetwork'));
    } finally { this.restarting = false; }
  }
  async _pump(res, gen, ctl) {
    this.streamCtl = ctl || null;
    this.streamPos = 0; this.eof = false;
    const reader = res.body.getReader();
    let cur = null, fill = 0, idx = 0;
    const keepAll = () => this.size != null && this.size <= STREAM_KEEP;
    const flush = (final) => {
      if (!cur || (!fill && !final)) return;
      const data = fill === BLOCK ? cur : cur.slice(0, fill);
      if (fill || final) {
        if (!this.blocks.has(idx)) this._put(idx, data);
        else { const w = this.waiters.get(idx); if (w) { this.waiters.delete(idx); w.forEach((x) => x.resolve(this.blocks.get(idx))); } }
      }
      if (!keepAll()) {   // file grande: tieni solo una finestra intorno alla lettura (+ LRU sul resto)
        for (const k of [...this.blocks.keys()]) if (k < this.readHead - STREAM_BACK && k !== 0) this._drop(k);
        this._evict(idx);
      }
      idx++; cur = null; fill = 0;
    };
    try {
      for (;;) {
        if (gen !== this.streamGen || this.closed) { try { reader.cancel(); } catch {} return; }
        // contropressione: non correre troppo avanti rispetto al player (file grandi)
        while (!keepAll() && idx - this.readHead > STREAM_AHEAD && gen === this.streamGen && !this.closed) {
          await new Promise((r) => { this.resumeStream = r; setTimeout(r, 1000); });
        }
        const { done, value } = await reader.read();
        if (done) break;
        this.stats.bytes += value.length;
        let o = 0;
        while (o < value.length) {
          if (!cur) {
            // un blocco già in cache (flusso ripartito) si salta senza copiarlo
            cur = new Uint8Array(BLOCK); fill = 0;
          }
          const n = Math.min(BLOCK - fill, value.length - o);
          cur.set(value.subarray(o, o + n), fill);
          fill += n; o += n; this.streamPos += n;
          if (fill === BLOCK) flush(false);
        }
      }
      if (gen !== this.streamGen) return;
      flush(true);
      this.eof = true;
      if (this.size == null || this.size !== this.streamPos) this.size = this.streamPos;
      // blocchi richiesti oltre la fine: vuoti
      for (const [i, arr] of [...this.waiters]) {
        if (i * BLOCK >= this.size) { this.waiters.delete(i); arr.forEach((x) => x.resolve(new Uint8Array(0))); }
        else if (!this.blocks.has(i)) this._restartStream();
      }
      this.dispatchEvent(new Event('progress'));
    } catch (e) {
      if (gen !== this.streamGen || this.closed) return;
      // connessione caduta a metà: si riparte (i blocchi già presi restano in cache)
      this.stats.retries++;
      await sleep(600);
      if (gen === this.streamGen && !this.closed) this._restartStream();
    }
  }

  close() {
    super.close();
    for (const [, f] of this.inflight) f.ctrl.abort();
    this.inflight.clear(); this.queue = [];
    this.streamGen++;
    if (this.streamCtl) this.streamCtl.abort();
    this._kickStream();
  }
}
