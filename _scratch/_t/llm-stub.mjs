
export async function loadCfg(){ return { provider:'openai', base:'https://api.x.ai/v1', model:'grok-2-latest', key:'x' }; }
export function brandOf(){ return 'Grok'; }
export function providerLabel(){ return 'Grok (grok-2-latest)'; }
export function endpointHost(){ return 'api.x.ai'; }
export function extractJson(x){ if(!x) return null; const a=x.indexOf('{'),b=x.lastIndexOf('}'); if(a<0||b<=a) return null; try{return JSON.parse(x.slice(a,b+1));}catch{return null;} }
export async function ask(){ return (globalThis.__ASK ? globalThis.__ASK() : '{"cell":0}'); }
