// nearby-skill.js — the scoped ANIMA skill spec for "Vicino" (device-to-device transfer).
//
// PURE + dependency-injected: it exports buildNearbySpec(ops) returning a spec for anima-skill.js's
// makeSkill(); the app composes `makeSkill(buildNearbySpec(ops))`. No imports / no DOM / no fetch, so
// the host gate (tools/anima-host/nearby-check.mjs) tests it directly. `ops` is the seam to the device
// (the web app wires it to /api/link/*; the test injects fakes).
//
// Safety follows the federated-skill contract (apps/anima/www/anima-skill.js): a CLOSED action set, and
// the device-touching verbs (send a file, send a command, accept an incoming transfer) are mutating →
// they require the app's confirm() before they run. So even an injected/hallucinated "send my keys to
// X" cannot fire silently — exactly the gate Bruce's arbitrary-exec lacks.

// Deterministic floor: unambiguous IT/EN commands resolve with no model.
export function parseNearby(text) {
  const q = String(text || '').trim();
  const low = q.toLowerCase();

  // "manda il comando X a <peer>" / "send command X to <peer>" — checked BEFORE the bare file form so
  // "manda il comando reboot a Marco" is a command, not a file named "il comando reboot". Match on the
  // ORIGINAL text (regex is /i) so file/peer/command keep their original casing.
  let m = q.match(/^(?:invia|manda|send)\s+(?:il\s+)?comando\s+["']?(.+?)["']?\s+(?:a|al|to)\s+["']?(.+?)["']?[.!]?$/i)
       || q.match(/^(?:run|esegui)\s+["']?(.+?)["']?\s+(?:su|on|a|to)\s+["']?(.+?)["']?[.!]?$/i);
  if (m && m[1] && m[2]) return { action: 'nearby_send_cmd', args: { command: m[1].trim(), peer: m[2].trim() } };

  // "invia <file> a <peer>" / "send <file> to <peer>"
  m = q.match(/^(?:invia|inviami|manda|mandami|spedisci|send|share)\s+(?:il\s+file\s+|the\s+file\s+|file\s+)?["']?(.+?)["']?\s+(?:a|ad|al|alla|allo|to)\s+["']?(.+?)["']?[.!]?$/i);
  if (m && m[1] && m[2]) return { action: 'nearby_send_file', args: { file: m[1].trim(), peer: m[2].trim() } };

  // discovery
  if (/\b(cerca|trova|scansiona|scan|find|discover)\b/.test(low) && /\b(dispositiv|device|peer|vicin|nearby)/.test(low))
    return { action: 'nearby_discover', args: {} };

  // protocol switch — require a "mode/protocol" word (so "manda file a bruce" stays a send, not a switch).
  // NB: no \b around "modalità" — JS \b uses ASCII boundaries and fails right after the accented 'à'.
  const wantsMode = /(modalit|mode|protocoll|protocol)/i.test(low);
  if (wantsMode && /\bbruce\b/i.test(low))  return { action: 'nearby_set_proto', args: { proto: 'bruce' } };
  if (wantsMode && /\bnucleo\b/i.test(low)) return { action: 'nearby_set_proto', args: { proto: 'nucleo' } };

  // accept / reject a pending incoming offer
  if (/\b(accetta|accept|ricevi|ok)\b/.test(low) && /\b(file|offerta|offer|trasferimento|transfer)\b/.test(low)) return { action: 'nearby_accept', args: { accept: true } };
  if (/\b(rifiuta|reject|annulla|no)\b/.test(low) && /\b(file|offerta|offer|trasferimento|transfer)\b/.test(low)) return { action: 'nearby_accept', args: { accept: false } };

  return null;
}

const SYS = {
  it: `Sei l'assistente di "Vicino", l'app che scambia file e comandi con dispositivi vicini via ESP-NOW.
Puoi SOLO: cercare dispositivi, inviare un file a un dispositivo, inviare un comando (che l'altro deve confermare), cambiare protocollo (Nucleo/Bruce), accettare/rifiutare un'offerta in arrivo. Rifiuta tutto il resto.`,
  en: `You are the assistant of "Vicino", the app that exchanges files and commands with nearby devices over ESP-NOW.
You may ONLY: discover devices, send a file to a device, send a command (the peer must confirm it), switch protocol (Nucleo/Bruce), accept/reject an incoming offer. Refuse everything else.`,
};

export function buildNearbySpec(ops, opts = {}) {
  const lang = opts.lang === 'en' ? 'en' : 'it';
  return {
    id: 'nearby', label: 'Vicino',
    systemPrompt: SYS[lang],
    missReply: lang === 'en' ? '(no Vicino command recognised)' : '(nessun comando Vicino riconosciuto)',
    deniedReply: lang === 'en' ? '(needs your confirmation)' : '(richiede la tua conferma)',
    actions: [
      { name: 'nearby_discover', description: 'Scan for nearby devices', schema: { type: 'object', properties: {} } },
      { name: 'nearby_set_proto', description: 'Switch transfer protocol', schema: { type: 'object', properties: { proto: { type: 'string', enum: ['nucleo', 'bruce'] } }, required: ['proto'] } },
      { name: 'nearby_send_file', description: 'Send a file from SD to a peer', mutating: true, schema: { type: 'object', properties: { file: { type: 'string' }, peer: { type: 'string' } }, required: ['file', 'peer'] } },
      { name: 'nearby_send_cmd', description: 'Send a command to a peer (peer confirms before it runs)', mutating: true, schema: { type: 'object', properties: { command: { type: 'string' }, peer: { type: 'string' } }, required: ['command', 'peer'] } },
      { name: 'nearby_accept', description: 'Accept or reject the pending incoming offer', mutating: true, schema: { type: 'object', properties: { accept: { type: 'boolean' } }, required: ['accept'] } },
    ],
    floor: (text) => parseNearby(text),
    validate: (action, args) => {
      if (action === 'nearby_set_proto' && !['nucleo', 'bruce'].includes(args.proto)) return { ok: false, error: lang === 'en' ? 'protocol must be nucleo or bruce' : 'protocollo deve essere nucleo o bruce' };
      if (action === 'nearby_send_file' && (!args.file || !args.peer)) return { ok: false, error: 'file and peer required' };
      if (action === 'nearby_send_cmd' && (!args.command || !args.peer)) return { ok: false, error: 'command and peer required' };
      if (action === 'nearby_accept' && typeof args.accept !== 'boolean') return { ok: false, error: 'accept must be boolean' };
      return { ok: true };
    },
    execute: async (action, args) => {
      switch (action) {
        case 'nearby_discover':  return ops.discover();
        case 'nearby_set_proto': return ops.setProto(args.proto);
        case 'nearby_send_file': return ops.sendFileByName(args.file, args.peer);
        case 'nearby_send_cmd':  return ops.sendCmdTo(args.peer, args.command);
        case 'nearby_accept':    return ops.answerOffer(!!args.accept);
        default: return { ok: false, reply: '(unknown action)' };
      }
    },
  };
}

export default buildNearbySpec;
