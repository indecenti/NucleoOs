var I=n=>{let e=n.reduce((a,s)=>a+s.length,0),t=new Uint8Array(e);t.set(n[0],0);for(let a=1;a<n.length;a++)t.set(n[a],n[a-1].length);return t},O=new TextDecoder,E=n=>O.decode(n);var F=n=>{n.every(t=>!!t.name)&&n.sort((a,s)=>a.name.localeCompare(s.name))};var m=n=>new URL(n,document.baseURI).href,M=(n,e)=>Array(Math.max(e-String(n).length+1,0)).join("0")+n;var A=()=>(async n=>{try{return typeof MessageChannel<"u"&&new MessageChannel().port1.postMessage(new SharedArrayBuffer(1)),WebAssembly.validate(n)}catch{return!1}})(new Uint8Array([0,97,115,109,1,0,0,0,1,4,1,96,0,0,3,2,1,0,5,4,1,3,1,1,10,11,1,9,0,65,0,254,16,2,0,26,11])),P=async()=>WebAssembly.validate(new Uint8Array([0,97,115,109,1,0,0,0,1,4,1,96,0,0,3,2,1,0,10,8,1,6,0,6,64,25,11,11])),W=async()=>WebAssembly.validate(new Uint8Array([0,97,115,109,1,0,0,0,1,5,1,96,0,1,123,3,2,1,0,10,10,1,8,0,65,0,253,15,253,98,11])),v=async()=>{if(!await P())throw new Error("WebAssembly runtime does not support exception handling");if(!await W())throw new Error("WebAssembly runtime does not support SIMD")},L=()=>w()||!!navigator.userAgent.match(/Version\/([0-9\._]+).*Safari/),w=()=>!!navigator.userAgent.match(/Version\/([0-9\._]+).*Mobile.*Safari.*/);var U=`
const fsNameToFile = {};  // map Name => File
const fsIdToFile = {};    // map ID => File
let currFileId = 0;

// Patch and redirect memfs calls to wllama
const patchMEMFS = () => {
  const m = wModule;
  // save functions
  m.MEMFS.stream_ops._read = m.MEMFS.stream_ops.read;
  m.MEMFS.stream_ops._write = m.MEMFS.stream_ops.write;
  m.MEMFS.stream_ops._llseek = m.MEMFS.stream_ops.llseek;
  m.MEMFS.stream_ops._allocate = m.MEMFS.stream_ops.allocate;
  m.MEMFS.stream_ops._mmap = m.MEMFS.stream_ops.mmap;
  m.MEMFS.stream_ops._msync = m.MEMFS.stream_ops.msync;

  const patchStream = (stream) => {
    const name = stream.node.name;
    if (fsNameToFile[name]) {
      const f = fsNameToFile[name];
      stream.node.contents = m.HEAPU8.subarray(f.ptr, f.ptr + f.size);
      stream.node.usedBytes = f.size;
    }
  };

  // replace "read" functions
  m.MEMFS.stream_ops.read = function (stream, buffer, offset, length, position) {
    patchStream(stream);
    return m.MEMFS.stream_ops._read(stream, buffer, offset, length, position);
  };
  m.MEMFS.ops_table.file.stream.read = m.MEMFS.stream_ops.read;

  // replace "llseek" functions
  m.MEMFS.stream_ops.llseek = function (stream, offset, whence) {
    patchStream(stream);
    return m.MEMFS.stream_ops._llseek(stream, offset, whence);
  };
  m.MEMFS.ops_table.file.stream.llseek = m.MEMFS.stream_ops.llseek;

  // replace "mmap" functions
  m.MEMFS.stream_ops.mmap = function (stream, length, position, prot, flags) {
    patchStream(stream);
    const name = stream.node.name;
    if (fsNameToFile[name]) {
      const f = fsNameToFile[name];
      return {
        ptr: f.ptr + position,
        allocated: false,
      };
    } else {
      return m.MEMFS.stream_ops._mmap(stream, length, position, prot, flags);
    }
  };
  m.MEMFS.ops_table.file.stream.mmap = m.MEMFS.stream_ops.mmap;

  // mount FS
  m.FS.mkdir('/models');
  m.FS.mount(m.MEMFS, { root: '.' }, '/models');
};

// Allocate a new file in wllama heapfs, returns file ID
const heapfsAlloc = (name, size) => {
  if (size < 1) {
    throw new Error('File size must be bigger than 0');
  }
  const m = wModule;
  const ptr = m.mmapAlloc(size);
  const file = {
    ptr: ptr,
    size: size,
    id: currFileId++,
  };
  fsIdToFile[file.id] = file;
  fsNameToFile[name] = file;
  return file.id;
};

// Add new file to wllama heapfs, return number of written bytes
const heapfsWrite = (id, buffer, offset) => {
  const m = wModule;
  if (fsIdToFile[id]) {
    const { ptr, size } = fsIdToFile[id];
    const afterWriteByte = offset + buffer.byteLength;
    if (afterWriteByte > size) {
      throw new Error(\`File ID \${id} write out of bound, afterWriteByte = \${afterWriteByte} while size = \${size}\`);
    }
    m.HEAPU8.set(buffer, ptr + offset);
    return buffer.byteLength;
  } else {
    throw new Error(\`File ID \${id} not found in heapfs\`);
  }
};
`,N=`
// send message back to main thread
const msg = (data) => postMessage(data);

// Convert CPP log into JS log
const cppLogToJSLog = (line) => {
  const matched = line.match(/@@(DEBUG|INFO|WARN|ERROR)@@(.*)/);
  return !!matched
    ? {
      level: (matched[1] === 'INFO' ? 'debug' : matched[1]).toLowerCase(),
      text: matched[2],
    }
    : { level: 'log', text: line };
};

// Get module config that forwards stdout/err to main thread
const getWModuleConfig = (pathConfig, pthreadPoolSize) => {
  if (!pathConfig['wllama.js']) {
    throw new Error('"wllama.js" is missing in pathConfig');
  }
  return {
    noInitialRun: true,
    print: function (text) {
      if (arguments.length > 1) text = Array.prototype.slice.call(arguments).join(' ');
      msg({ verb: 'console.log', args: [text] });
    },
    printErr: function (text) {
      if (arguments.length > 1) text = Array.prototype.slice.call(arguments).join(' ');
      const logLine = cppLogToJSLog(text);
      msg({ verb: 'console.' + logLine.level, args: [logLine.text] });
    },
    locateFile: function (filename, basePath) {
      const p = pathConfig[filename];
      const truncate = (str) => str.length > 128 ? \`\${str.substr(0, 128)}...\` : str;
      msg({ verb: 'console.debug', args: [\`Loading "\${filename}" from "\${truncate(p)}"\`] });
      return p;
    },
    mainScriptUrlOrBlob: pathConfig['wllama.js'],
    pthreadPoolSize,
    wasmMemory: pthreadPoolSize > 1 ? getWasmMemory() : null,
    onAbort: function (text) {
      msg({ verb: 'signal.abort', args: [text] });
    },
  };
};

// Get the memory to be used by wasm. (Only used in multi-thread mode)
// Because we have a weird OOM issue on iOS, we need to try some values
// See: https://github.com/emscripten-core/emscripten/issues/19144
//      https://github.com/godotengine/godot/issues/70621
const getWasmMemory = () => {
  let minBytes = 128 * 1024 * 1024;
  let maxBytes = 4096 * 1024 * 1024;
  let stepBytes = 128 * 1024 * 1024;
  while (maxBytes > minBytes) {
    try {
      const wasmMemory = new WebAssembly.Memory({
        initial: minBytes / 65536,
        maximum: maxBytes / 65536,
        shared: true,
      });
      return wasmMemory;
    } catch (e) {
      maxBytes -= stepBytes;
      continue; // retry
    }
  }
  throw new Error('Cannot allocate WebAssembly.Memory');
};
`,j=`
// Start the main llama.cpp
let wModule;
let wllamaStart;
let wllamaAction;
let wllamaExit;
let wllamaDebug;

${N}

${U}

const callWrapper = (name, ret, args) => {
  const fn = wModule.cwrap(name, ret, args);
  return async (action, req) => {
    let result;
    try {
      if (args.length === 2) {
        result = await fn(action, req);
      } else {
        result = fn();
      }
    } catch (ex) {
      console.error(ex);
      throw ex;
    }
    return result;
  };
}

onmessage = async (e) => {
  if (!e.data) return;
  const { verb, args, callbackId } = e.data;

  if (!callbackId) {
    msg({ verb: 'console.error', args: ['callbackId is required', e.data] });
    return;
  }

  if (verb === 'module.init') {
    const argPathConfig      = args[0];
    const argPThreadPoolSize = args[1];
    try {
      const Module = ModuleWrapper();
      wModule = await Module(getWModuleConfig(
        argPathConfig,
        argPThreadPoolSize,
      ));

      // init FS
      patchMEMFS();

      // init cwrap
      wllamaStart  = callWrapper('wllama_start' , 'string', []);
      wllamaAction = callWrapper('wllama_action', 'string', ['string', 'string']);
      wllamaExit   = callWrapper('wllama_exit'  , 'string', []);
      wllamaDebug  = callWrapper('wllama_debug' , 'string', []);
      msg({ callbackId, result: null });

    } catch (err) {
      msg({ callbackId, err });
    }
    return;
  }

  if (verb === 'fs.alloc') {
    const argFilename = args[0];
    const argSize     = args[1];
    try {
      // create blank file
      const emptyBuffer = new ArrayBuffer(0);
      wModule['FS_createDataFile']('/models', argFilename, emptyBuffer, true, true, true);
      // alloc data on heap
      const fileId = heapfsAlloc(argFilename, argSize);
      msg({ callbackId, result: { fileId } });
    } catch (err) {
      msg({ callbackId, err });
    }
    return;
  }

  if (verb === 'fs.write') {
    const argFileId = args[0];
    const argBuffer = args[1];
    const argOffset = args[2];
    try {
      const writtenBytes = heapfsWrite(argFileId, argBuffer, argOffset);
      msg({ callbackId, result: { writtenBytes } });
    } catch (err) {
      msg({ callbackId, err });
    }
    return;
  }

  if (verb === 'wllama.start') {
    try {
      const result = await wllamaStart();
      msg({ callbackId, result });
    } catch (err) {
      msg({ callbackId, err });
    }
    return;
  }

  if (verb === 'wllama.action') {
    const argAction = args[0];
    const argBody = args[1];
    try {
      const result = await wllamaAction(argAction, argBody);
      msg({ callbackId, result });
    } catch (err) {
      msg({ callbackId, err });
    }
    return;
  }

  if (verb === 'wllama.exit') {
    try {
      const result = await wllamaExit();
      msg({ callbackId, result });
    } catch (err) {
      msg({ callbackId, err });
    }
    return;
  }

  if (verb === 'wllama.debug') {
    try {
      const result = await wllamaDebug();
      msg({ callbackId, result });
    } catch (err) {
      msg({ callbackId, err });
    }
    return;
  }
};
`,T=class{logger;suppressNativeLog;taskQueue=[];taskId=1;resultQueue=[];busy=!1;worker;pathConfig;multiThread;nbThread;constructor(e,t=1,a,s){this.pathConfig=e,this.nbThread=t,this.multiThread=t>1,this.logger=s,this.suppressNativeLog=a}async moduleInit(e){if(!this.pathConfig["wllama.js"])throw new Error('"single-thread/wllama.js" or "multi-thread/wllama.js" is missing from pathConfig');let a=(await import(this.pathConfig["wllama.js"])).default.toString();a=a.replace(/import\.meta/g,"importMeta");let s=["const importMeta = {}",`function ModuleWrapper() {
        const _scriptDir = ${JSON.stringify(window.location.href)};
        return ${a};
      }`,j].join(`;

`),r=window.URL.createObjectURL(new Blob([s],{type:"text/javascript"}));this.worker=new Worker(r),this.worker.onmessage=this.onRecvMsg.bind(this),this.worker.onerror=this.logger.error;let o=await this.pushTask({verb:"module.init",args:[this.pathConfig,this.nbThread],callbackId:this.taskId++}),i=[];for(let l of e){let d=await this.fileAlloc(l.name,l.blob.size);i.push({id:d,...l})}return await Promise.all(i.map(l=>this.fileWrite(l.id,l.blob))),o}async wllamaStart(){let e=await this.pushTask({verb:"wllama.start",args:[],callbackId:this.taskId++});return this.parseResult(e)}async wllamaAction(e,t){let a=await this.pushTask({verb:"wllama.action",args:[e,JSON.stringify(t)],callbackId:this.taskId++});return this.parseResult(a)}async wllamaExit(){if(this.worker){let e=await this.pushTask({verb:"wllama.exit",args:[],callbackId:this.taskId++});this.parseResult(e),this.worker.terminate()}}async wllamaDebug(){let e=await this.pushTask({verb:"wllama.debug",args:[],callbackId:this.taskId++});return JSON.parse(e)}async fileAlloc(e,t){return(await this.pushTask({verb:"fs.alloc",args:[e,t],callbackId:this.taskId++})).fileId}async fileWrite(e,t){let a=t.stream().getReader(),s=0;for(;;){let{done:r,value:o}=await a.read();if(r)break;let i=o.byteLength;await this.pushTask({verb:"fs.write",args:[e,o,s],callbackId:this.taskId++},[o.buffer]),s+=i}}parseResult(e){let t=JSON.parse(e);if(t&&t.__exception)throw new Error(t.__exception);return t}pushTask(e,t){return new Promise((a,s)=>{this.taskQueue.push({resolve:a,reject:s,param:e,buffers:t}),this.runTaskLoop()})}async runTaskLoop(){if(!this.busy){for(this.busy=!0;;){let e=this.taskQueue.shift();if(!e)break;this.resultQueue.push(e),this.worker.postMessage(e.param,w()?void 0:{transfer:e.buffers??[]})}this.busy=!1}}onRecvMsg(e){if(!e.data)return;let{verb:t,args:a}=e.data;if(t&&t.startsWith("console.")){if(this.suppressNativeLog)return;t.endsWith("debug")&&this.logger.debug(...a),t.endsWith("log")&&this.logger.log(...a),t.endsWith("warn")&&this.logger.warn(...a),t.endsWith("error")&&this.logger.error(...a);return}else t==="signal.abort"&&this.abort(a[0]);let{callbackId:s,result:r,err:o}=e.data;if(s){let i=this.resultQueue.findIndex(l=>l.param.callbackId===s);if(i!==-1){let l=this.resultQueue.splice(i,1)[0];o?l.reject(o):l.resolve(r)}else this.logger.error(`Cannot find waiting task with callbackId = ${s}`)}}abort(e){for(;this.resultQueue.length>0;){let t=this.resultQueue.pop();if(!t)break;t.reject(new Error(`Received abort signal from llama.cpp; Message: ${e||"(empty)"}`))}}};var p="__metadata__",C="polyfill_for_older_version",x=class{async getNameFromURL(e){return await S(e,"")}async write(e,t,a){return this.writeMetadata(e,a),await R(e,t)}async open(e){return await z(e)}async getSize(e){return await $(e)}async getMetadata(e){let t=await z(e,p),a=await this.getSize(e);if(!t)return a>0?{etag:C,originalSize:a,originalURL:""}:null;try{return await new Response(t).json()}catch{return null}}async list(){let e=await b(),t=[],a={};for await(let[s,r]of e.entries())if(r.kind==="file"&&s.startsWith(p)){let o=(await r.getFile()).stream(),i=await new Response(o).json().catch(l=>null);a[s.replace(p,"")]=i}for await(let[s,r]of e.entries())r.kind==="file"&&!s.startsWith(p)&&t.push({name:s,size:await r.getFile().then(o=>o.size),metadata:a[s]||{originalSize:(await r.getFile()).size,originalURL:"",etag:""}});return t}async clear(){await this.deleteMany(()=>!0)}async delete(e){let t=await this.getNameFromURL(e);await this.deleteMany(a=>a.name===e||a.name===t)}async deleteMany(e){let t=await b(),a=await this.list();for(let s of a)e(s)&&t.removeEntry(s.name)}async writeMetadata(e,t){let a=new Blob([JSON.stringify(t)],{type:"text/plain"});await R(e,a.stream(),p)}},D=x;async function R(n,e,t=""){try{let a=await b(),s=await S(n,t),r=L()?await K(s):await a.getFileHandle(s,{create:!0}).then(i=>i.createWritable());await r.truncate(0);let o=e.getReader();for(;;){let{done:i,value:l}=await o.read();if(i)break;await r.write(l)}await r.close()}catch(a){console.error("opfsWrite",a)}}async function z(n,e=""){try{let t=await b(),a=await S(n,e);return(await(await t.getFileHandle(a)).getFile()).stream()}catch{return null}}async function $(n,e=""){try{let t=await b(),a=await S(n,e);return(await(await t.getFileHandle(a)).getFile()).size}catch{return-1}}async function S(n,e){let t=await crypto.subtle.digest("SHA-1",new TextEncoder().encode(n)),s=Array.from(new Uint8Array(t)).map(r=>r.toString(16).padStart(2,"0")).join("");return`${e}${s}_${n.split("/").pop()}`}async function b(){return await(await navigator.storage.getDirectory()).getDirectoryHandle("cache",{create:!0})}var H=`
const msg = (data) => postMessage(data);
let accessHandle;

onmessage = async (e) => {
  try {
    if (!e.data) return;
    const {
      open,  // name of file to open
      value, // value to be written
      done,  // indicates when to close the file
    } = e.data;

    if (open) {
      const opfsRoot = await navigator.storage.getDirectory();
      const cacheDir = await opfsRoot.getDirectoryHandle('cache', { create: true });
      const fileHandler = await cacheDir.getFileHandle(open, { create: true });
      accessHandle = await fileHandler.createSyncAccessHandle();
      accessHandle.truncate(0); // clear file content
      return msg({ ok: true });

    } else if (value) {
      accessHandle.write(value);
      return msg({ ok: true });

    } else if (done) {
      accessHandle.flush();
      accessHandle.close();
      return msg({ ok: true });
    }

    throw new Error('OPFS Worker: Invalid state');
  } catch (err) {
    return msg({ err });
  }
};
`;async function K(n){let e=window.URL.createObjectURL(new Blob([H],{type:"text/javascript"})),t=new Worker(e),a,s;t.onmessage=o=>{o.data.ok?a(null):o.data.err&&s(o.data.err)};let r=o=>new Promise((i,l)=>{a=i,s=l,t.postMessage(o,w()?void 0:{transfer:o.value?[o.value.buffer]:[]})});return await r({open:n}),{truncate:async()=>{},write:o=>r({value:o}),close:async()=>{await r({done:!0}),t.terminate()}}}var _=class n extends Blob{static async create(e,t){let{cacheManager:a}=t,s=t?.fetch??fetch,r=e,o;try{let u=await s(e,{method:"HEAD"});o={originalURL:e,originalSize:Number(u.headers.get("content-length")),etag:(u.headers.get("etag")||"").replace(/[^A-Za-z0-9]/g,"")}}catch(u){if(t.allowOffline){let y=await a.getMetadata(r);if(y)o=y;else throw new Error("Network error, cannot find requested model in cache for using offline")}else throw u}let i=await a.getSize(r),l=await a.getMetadata(r),d=t?.useCache===!1,f=l?.etag===C;if(f&&await a.writeMetadata(r,o),(f||l&&o.etag===l.etag&&o.originalSize===i)&&!d){t?.logger?.debug(`Using cached file ${r}`);let u=await a.open(r);return(t?.startSignal??Promise.resolve()).then(()=>{t?.progressCallback?.({loaded:i,total:i})}),new n(e,0,o.originalSize,!0,s,{cachedStream:u,progressCallback:()=>{},etag:o.etag,noTEE:t.noTEE,cacheManager:a})}else return o.originalSize!==i&&t?.logger?.debug(`Cache file is present, but size mismatch (cache = ${i} bytes, remote = ${o.originalSize} bytes)`),l&&o.etag!==l.etag&&t?.logger?.debug(`Cache file is present, but ETag mismatch (cache = "${l.etag}", remote = "${o.etag}")`),t?.logger?.debug(`NOT using cache for ${r}`),new n(e,0,o.originalSize,!0,s,{progressCallback:t?.progressCallback??(()=>{}),startSignal:t?.startSignal,etag:o.etag,noTEE:t.noTEE,cacheManager:a})}cacheManager;url;etag;start;end;contentType="";full;fetch;cachedStream;progressCallback;startSignal;noTEE;constructor(e,t,a,s,r,o){if(super([]),t!==0)throw new Error("start range must be 0");this.url=e,this.start=t,this.end=a,this.contentType="",this.full=s,this.fetch=r,this.cachedStream=o.cachedStream,this.progressCallback=o.progressCallback,this.startSignal=o.startSignal,this.etag=o.etag,this.noTEE=o.noTEE,this.cacheManager=o.cacheManager}get size(){return this.end-this.start}get type(){return this.contentType}slice(){throw new Error("Unsupported operation")}async arrayBuffer(){throw new Error("Unsupported operation")}async text(){throw new Error("Unsupported operation")}stream(){if(this.cachedStream)return this.cachedStream;let e=this,t=0,a=new TransformStream({transform(s,r){e.noTEE||r.enqueue(s),t+=s.byteLength,e.progressCallback({loaded:t,total:e.size})},flush(s){e.progressCallback({loaded:e.size,total:e.size})}});return(async()=>(this.startSignal&&await this.startSignal,this.fetchRange().then(s=>{let[r,o]=s.body.tee();r.pipeThrough(a),this.cacheManager.write(this.url,o,{originalSize:this.end,originalURL:this.url,etag:this.etag})}).catch(s=>a.writable.abort(s.message))))(),a.readable}fetchRange(){let e=this.fetch;return this.full?e(this.url):e(this.url,{headers:{Range:`bytes=${this.start}-${this.end-1}`}})}};var g;(function(n){n[n.READY=0]="READY",n[n.WORKING=1]="WORKING",n[n.FINISHED=2]="FINISHED"})(g||(g={}));var k=class{tasks;maxParallel;progressCallback;logger;useCache;totalBytes=0;allowOffline;noTEE;cacheManager;constructor(e,t,a,s,r){this.tasks=t.map(o=>{let i={url:o,state:g.READY,loaded:0};return i.signalStart=new Promise(l=>i.fireStart=l),i.signalEnd=new Promise(l=>i.fireEnd=l),i}),this.logger=e,this.maxParallel=a,this.progressCallback=r.progressCallback,this.useCache=r.useCache,this.allowOffline=r.allowOffline,this.noTEE=!!r.noTEE,this.cacheManager=s}async run(){await Promise.all(this.tasks.map(async e=>{e.blob=await _.create(e.url,{logger:this.logger,useCache:this.useCache,startSignal:e.signalStart,allowOffline:this.allowOffline,noTEE:this.noTEE,cacheManager:this.cacheManager,progressCallback:({loaded:t})=>{e.loaded=t,this.updateProgress(e)}})})),this.totalBytes=this.tasks.reduce((e,t)=>e+t.blob.size,0);for(let e=0;e<this.maxParallel;e++)this.dispatcher();return this.tasks.map(e=>e.blob)}updateProgress(e){let t={loaded:this.tasks.reduce((a,s)=>a+s.loaded,0),total:this.totalBytes};this.progressCallback?.(t),e.loaded===e.blob.size&&(e.state=g.FINISHED,e.fireEnd())}async dispatcher(){for(;;){let e=this.tasks.find(t=>t.state===g.READY);if(!e)return;e.state=g.WORKING,e.fireStart(),await e.signalEnd}}};var oe={...console,debug:()=>{}},c=class extends Error{type;constructor(e,t="unknown_error"){super(e),this.type=t}},B=class{cacheManager;proxy=null;config;pathConfig;useMultiThread=!1;useEmbeddings=!1;loadedContextInfo=null;bosToken=-1;eosToken=-1;eotToken=-1;addBosToken=!1;addEosToken=!1;chatTemplate;metadata;samplingConfig={};hasEncoder=!1;decoderStartToken=-1;nCachedTokens=0;constructor(e,t={}){if(v(),!e)throw new c("AssetsPathConfig is required");this.pathConfig=e,this.config=t,this.cacheManager=t.cacheManager??new D}logger(){return this.config.logger??console}checkModelLoaded(){if(!this.isModelLoaded())throw new c("loadModel() is not yet called","model_not_loaded")}isModelLoaded(){return!!this.proxy&&!!this.metadata}getBOS(){return this.bosToken}getEOS(){return this.eosToken}getEOT(){return this.eotToken}getDecoderStartToken(){return this.decoderStartToken}getModelMetadata(){return this.checkModelLoaded(),this.metadata}isMultithread(){return this.checkModelLoaded(),this.useMultiThread}isEncoderDecoderArchitecture(){return this.checkModelLoaded(),this.hasEncoder}mustAddBosToken(){return this.checkModelLoaded(),this.addBosToken}mustAddEosToken(){return this.checkModelLoaded(),this.addEosToken}getChatTemplate(){return this.checkModelLoaded(),this.chatTemplate??null}parseModelUrl(e){if(Array.isArray(e))return e;let t=/(?<baseURL>.*)-(?<current>\d{5})-of-(?<total>\d{5})\.gguf$/,a=e.match(t);if(!a||!a.groups||Object.keys(a.groups).length!==3)return[e];let{baseURL:s,total:r}=a.groups;return Array.from({length:Number(r)},(i,l)=>(l+1).toString().padStart(5,"0")).map(i=>`${s}-${i}-of-${r}.gguf`)}async downloadModel(e,t={}){if(e.length===0)throw new c("modelUrl must be an URL or a list of URLs (in the correct order)","download_error");if(t.useCache===!1)throw new c("useCache must not be false","download_error");let s=await new k(this.logger(),this.parseModelUrl(e),t.parallelDownloads??3,this.cacheManager,{progressCallback:t.progressCallback,useCache:!0,allowOffline:!!t.allowOffline,noTEE:!0}).run();await Promise.all(s.map(async r=>{let o=r.stream().getReader();for(;;){let{done:i}=await o.read();if(i)return}}))}async loadModelFromUrl(e,t={}){if(e.length===0)throw new c("modelUrl must be an URL or a list of URLs (in the correct order)","load_error");let a=t.useCache===!1,r=await new k(this.logger(),this.parseModelUrl(e),t.parallelDownloads??3,this.cacheManager,{progressCallback:t.progressCallback,useCache:!a,allowOffline:!!t.allowOffline}).run();return await this.loadModel(r,t)}async loadModel(e,t={}){let a=[...e];if(a.some(u=>u.size===0))throw new c("Input model (or splits) must be non-empty Blob or File","load_error");F(a);let s=a.length>1;if(this.proxy)throw new c("Module is already initialized","load_error");let r=await A();r||this.logger().warn("Multi-threads are not supported in this environment, falling back to single-thread");let o=!!this.pathConfig["multi-thread/wllama.js"]&&!!this.pathConfig["multi-thread/wllama.wasm"]&&!!this.pathConfig["multi-thread/wllama.worker.mjs"];o||this.logger().warn('Missing paths to "wllama.js", "wllama.wasm" or "wllama.worker.mjs", falling back to single-thread');let i=Math.floor((navigator.hardwareConcurrency||1)/2),l=t.n_threads??i;this.useMultiThread=r&&o&&l>1;let d=this.useMultiThread?{"wllama.js":m(this.pathConfig["multi-thread/wllama.js"]),"wllama.wasm":m(this.pathConfig["multi-thread/wllama.wasm"]),"wllama.worker.mjs":m(this.pathConfig["multi-thread/wllama.worker.mjs"])}:{"wllama.js":m(this.pathConfig["single-thread/wllama.js"]),"wllama.wasm":m(this.pathConfig["single-thread/wllama.wasm"])};this.proxy=new T(d,this.useMultiThread?l:1,this.config.suppressNativeLog??!1,this.logger()),await this.proxy.moduleInit(a.map((u,y)=>({name:s?`model-${M(y+1,5)}-of-${M(a.length,5)}.gguf`:"model.gguf",blob:u})));let f=await this.proxy.wllamaStart();if(!f.success)throw new c(`Error while calling start function, result = ${f}`);let h=await this.proxy.wllamaAction("load",{...t,use_mmap:!0,use_mlock:!0,seed:t.seed||Math.floor(Math.random()*1e5),n_ctx:t.n_ctx||1024,n_threads:this.useMultiThread?l:1,model_path:s?`/models/model-00001-of-${M(a.length,5)}.gguf`:"/models/model.gguf"});this.bosToken=h.token_bos,this.eosToken=h.token_eos,this.eotToken=h.token_eot,this.useEmbeddings=!!t.embeddings,this.metadata={hparams:{nVocab:h.n_vocab,nCtxTrain:h.n_ctx_train,nEmbd:h.n_embd,nLayer:h.n_layer},meta:h.metadata},this.hasEncoder=!!h.has_encoder,this.decoderStartToken=h.token_decoder_start,this.addBosToken=h.add_bos_token,this.addEosToken=h.add_eos_token,this.chatTemplate=h.metadata["tokenizer.chat_template"],this.loadedContextInfo=h,this.logger().debug({loadResult:h})}getLoadedContextInfo(){if(this.checkModelLoaded(),!this.loadedContextInfo)throw new c("Loaded context info is not available");return{...this.loadedContextInfo}}async createEmbedding(e,t={}){this.checkModelLoaded();let a={skipBOS:!1,skipEOS:!1,...t};await this.samplingInit(this.samplingConfig),await this.kvClear();let s=await this.tokenize(e);return this.bosToken&&!a.skipBOS&&s.unshift(this.bosToken),this.eosToken&&!a.skipEOS&&s.push(this.eosToken),await this.embeddings(s)}async createCompletion(e,t){this.checkModelLoaded(),this.samplingConfig=t.sampling??{},await this.samplingInit(this.samplingConfig);let a=[this.eosToken,this.eotToken,...t.stopTokens??[]],s=await this.tokenize(e,!0);this.addBosToken&&s[0]!==this.bosToken&&s.unshift(this.bosToken),t.useCache?s=await this.computeNonCachedTokens(s):await this.kvClear(),await this.samplingAccept(s),this.isEncoderDecoderArchitecture()?(await this.encode(s),await this.decode([this.getDecoderStartToken()],{})):await this.decode(s,{});let r=new Uint8Array,o=!1,i=()=>{o=!0};for(let l=0;l<(t.nPredict??1/0);l++){let d=await this.samplingSample();if(a.includes(d.token)||(r=I([r,d.piece]),t.onNewToken&&t.onNewToken(d.token,d.piece,E(r),{abortSignal:i}),o))break;await this.samplingAccept([d.token]),await this.decode([d.token],{})}return E(r)}async samplingInit(e,t=[]){if(this.checkModelLoaded(),this.samplingConfig=e,!(await this.proxy.wllamaAction("sampling_init",{...e,tokens:t})).success)throw new c("Failed to initialize sampling")}async getVocab(){return this.checkModelLoaded(),(await this.proxy.wllamaAction("get_vocab",{})).vocab.map(t=>new Uint8Array(t))}async lookupToken(e){this.checkModelLoaded();let t=await this.proxy.wllamaAction("lookup_token",{piece:e});return t.success?t.token:-1}async tokenize(e,t=!0){return this.checkModelLoaded(),(await this.proxy.wllamaAction("tokenize",t?{text:e,special:!0}:{text:e})).tokens}async detokenize(e){this.checkModelLoaded();let t=await this.proxy.wllamaAction("detokenize",{tokens:e});return new Uint8Array(t.buffer)}async decode(e,t){if(this.checkModelLoaded(),this.useEmbeddings)throw new c("embeddings is enabled. Use wllama.setOptions({ embeddings: false }) to disable it.");if(e.length===0)return{nPast:this.nCachedTokens};if(this.nCachedTokens+e.length>this.loadedContextInfo.n_ctx)throw new c("Running out of context cache. Please increase n_ctx when loading the model","kv_cache_full");let a=this.breakTokensIntoBatches(e,this.loadedContextInfo.n_batch),s;for(let r=0;r<a.length;r++){let o=a.length>1&&r<a.length-1;if(s=await this.proxy.wllamaAction("decode",{tokens:a[r],skip_logits:t.skipLogits||o}),s.error)throw new c(s.error);if(!s.success)throw new c("Cannot encode, unknown error")}return this.nCachedTokens=s.n_past,{nPast:s.n_past}}async encode(e,t){if(this.checkModelLoaded(),!this.hasEncoder)throw new c("This model does not use encoder-decoder architecture.","inference_error");if(this.useEmbeddings)throw new c("embeddings is enabled. Use wllama.setOptions({ embeddings: false }) to disable it.","inference_error");if(e.length===0)return{nPast:this.nCachedTokens};if(this.nCachedTokens+e.length>this.loadedContextInfo.n_ctx)throw new c("Running out of context cache. Please increase n_ctx when loading the model","kv_cache_full");let a=this.breakTokensIntoBatches(e,this.loadedContextInfo.n_batch),s;for(let r=0;r<a.length;r++){if(s=await this.proxy.wllamaAction("encode",{tokens:a[r]}),s.error)throw new c(s.error);if(!s.success)throw new c("Cannot encode, unknown error")}return this.nCachedTokens=s.n_past,{nPast:s.n_past}}breakTokensIntoBatches(e,t){let a=[];for(let s=0;s<e.length;s+=t)a.push(e.slice(s,s+t));return a}async samplingSample(){this.checkModelLoaded();let e=await this.proxy.wllamaAction("sampling_sample",{});return{piece:new Uint8Array(e.piece),token:e.token}}async samplingAccept(e){if(this.checkModelLoaded(),!(await this.proxy.wllamaAction("sampling_accept",{tokens:e})).success)throw new c("samplingAccept unknown error")}async getLogits(e=40){return this.checkModelLoaded(),(await this.proxy.wllamaAction("get_logits",{top_k:e})).logits.map(([s,r])=>({token:s,p:r}))}async embeddings(e){if(this.checkModelLoaded(),!this.useEmbeddings)throw new c("embeddings is disabled. Use wllama.setOptions({ embeddings: true }) to enable it.","inference_error");if(this.nCachedTokens>0&&this.logger().warn("Embeddings: KV cache is not empty, this may produce incorrect results"),this.nCachedTokens+e.length>this.loadedContextInfo.n_ctx)throw new c("Running out of context cache. Please increase n_ctx when loading the model","kv_cache_full");if(e.length>this.loadedContextInfo.n_batch)throw new c("Embedding tokens does not fit into batch. Please increase n_batch when loading the model","inference_error");if(e.length>this.loadedContextInfo.n_ubatch)throw new c("Embedding tokens does not fit into physical batch. Please increase n_ubatch when loading the model","inference_error");let t=await this.proxy.wllamaAction("embeddings",{tokens:e});if(t.error)throw new c(t.error);if(t.success)return t.embeddings;throw new c("embeddings unknown error")}async kvRemove(e,t){if(this.checkModelLoaded(),!(await this.proxy.wllamaAction("kv_remove",{n_keep:e,n_discard:t})).success)throw new c("kvRemove unknown error");this.nCachedTokens-=t}async kvClear(){if(this.checkModelLoaded(),!(await this.proxy.wllamaAction("kv_clear",{})).success)throw new c("kvClear unknown error");this.nCachedTokens=0}async sessionSave(e){return this.checkModelLoaded(),await this.proxy.wllamaAction("session_save",{session_path:e})}async sessionLoad(e){this.checkModelLoaded();let t=await this.proxy.wllamaAction("session_load",{session_path:e});if(t.error)throw new c(t.error);if(!t.success)throw new c("sessionLoad unknown error");let a=await this.getCachedTokens();this.nCachedTokens=a.length}async setOptions(e){this.checkModelLoaded(),await this.proxy.wllamaAction("set_options",e),this.useEmbeddings=e.embeddings}async exit(){await this.proxy?.wllamaExit()}async _getDebugInfo(){return this.checkModelLoaded(),await this.proxy.wllamaDebug()}async getCachedTokens(){return this.checkModelLoaded(),(await this.proxy.wllamaAction("current_status",{})).tokens}async computeNonCachedTokens(e){let t=await this.getCachedTokens(),a=0;for(;a<Math.min(t.length,e.length)&&t[a]===e[a];a++);let s=t.length-a;return this.logger().debug(`Cache nKeep=${a} nDiscard=${s}`),s>0&&await this.kvRemove(a,s),e.slice(a,e.length)}};export{oe as LoggerWithoutDebug,C as POLYFILL_ETAG,B as Wllama,c as WllamaError};
