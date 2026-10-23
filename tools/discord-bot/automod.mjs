#!/usr/bin/env node
// One-shot (re-runnable) moderation setup for the NucleoOS community server. Everything here runs on
// Discord's side afterwards (AutoMod + server safety settings): no process of ours reads messages.
// Needs the bot to hold Manage Server + Manage Channels + Manage Roles. Env: DISCORD_TOKEN, DISCORD_GUILD_ID.
const TOKEN = process.env.DISCORD_TOKEN, GUILD = process.env.DISCORD_GUILD_ID;
if (!TOKEN || !GUILD) { console.error('DISCORD_TOKEN and DISCORD_GUILD_ID are required'); process.exit(1); }
const API = 'https://discord.com/api/v10';

async function rest(method, path, body) {
    const r = await fetch(API + path, { method, body: body && JSON.stringify(body),
        headers: { Authorization: `Bot ${TOKEN}`, 'Content-Type': 'application/json', 'User-Agent': 'DiscordBot (nucleoos-bot, 1.0)' } });
    if (r.status === 429) { await new Promise(s => setTimeout(s, 2000)); return rest(method, path, body); }
    if (!r.ok) throw new Error(`${method} ${path} -> ${r.status} ${await r.text()}`);
    return r.status === 204 ? null : r.json();
}

// ---- staff-only #mod-log where AutoMod posts what it blocked ----
const channels = await rest('GET', `/guilds/${GUILD}/channels`);
const roles = await rest('GET', `/guilds/${GUILD}/roles`);
const me = await rest('GET', '/users/@me');
const team = roles.find(r => r.name === 'Team');
if (!team) throw new Error('role "Team" missing: run the server layout setup first');
const VIEW = String(1 << 10), SEND = String(1 << 11);
const staffOnly = [
    { id: GUILD, type: 0, deny: VIEW, allow: '0' },
    { id: team.id, type: 0, allow: VIEW, deny: '0' },
    { id: me.id, type: 1, allow: String((1 << 10) | (1 << 11)), deny: '0' },
];
let staff = channels.find(c => c.type === 4 && c.name.toLowerCase() === 'staff');
if (!staff) staff = await rest('POST', `/guilds/${GUILD}/channels`, { name: 'Staff', type: 4, permission_overwrites: staffOnly });
let modlog = channels.find(c => c.type === 0 && c.name === 'mod-log');
if (!modlog) modlog = await rest('POST', `/guilds/${GUILD}/channels`, { name: 'mod-log', type: 0, parent_id: staff.id,
    topic: 'AutoMod reports: what was blocked, who, where. Team only.', permission_overwrites: staffOnly });

// ---- server safety settings ----
const general = channels.find(c => c.type === 0 && c.name === 'general');
await rest('PATCH', `/guilds/${GUILD}`, {
    explicit_content_filter: 2,              // scan images/embeds from ALL members
    verification_level: 2,                   // Medium: verified email + account older than 5 minutes
    default_message_notifications: 1,        // only @mentions notify
    ...(general ? { system_channel_id: general.id } : {}),
});

// ---- AutoMod rules ----
const block = msg => ({ type: 1, metadata: { custom_message: msg } });
const alert = { type: 2, metadata: { channel_id: modlog.id } };
const timeout = s => ({ type: 3, metadata: { duration_seconds: s } });

const RULES = [
    { name: 'Profanity, sexual content, slurs (Discord lists)', event_type: 1, trigger_type: 4,
      trigger_metadata: { presets: [1, 2, 3], allow_list: ['damn', 'hell', 'crap'] },
      actions: [block('Please keep it friendly: that message was blocked by the server filter.'), alert] },

    { name: 'Insults and slurs (IT, ES, FR, DE)', event_type: 1, trigger_type: 1,
      trigger_metadata: { keyword_filter: [
          // Italian
          'cazzo', 'cazzi', 'vaffanculo', 'fanculo', 'stronz*', 'coglion*', 'puttan*', 'troia', 'troie', 'merda',
          'porco dio', 'porcodio', 'dio cane', 'diocane', 'dio porco', 'minchia', 'bastard*', 'frocio', 'froci',
          'ricchion*', 'negro', 'negri', 'mongoloid*', 'ritardat*', 'zoccola', 'figlio di puttana',
          // Spanish
          'puta', 'putas', 'puto', 'joder', 'cabron', 'cabrón', 'gilipollas', 'coño', 'maricon', 'maricón',
          'hijo de puta', 'pendejo', 'mierda', 'culero', 'chinga*',
          // French
          'putain', 'merde', 'connard', 'connasse', 'salope', 'encule', 'enculé', 'pédé', 'nique ta mere',
          'nique ta mère', 'fils de pute', 'batard', 'bâtard',
          // German
          'scheiße', 'scheisse', 'arschloch', 'fotze', 'hurensohn', 'wichser', 'schlampe', 'missgeburt',
          'schwuchtel', 'spast*', 'kanake',
      ], allow_list: [] },
      actions: [block('Please keep it friendly: that message was blocked by the server filter.'), alert] },

    { name: 'Piracy, malware and illegal trade', event_type: 1, trigger_type: 1,
      trigger_metadata: { keyword_filter: [
          '*romsfun*', '*romsmania*', '*emuparadise*', '*coolrom*', '*vimm.net*', '*cdromance*', '*romspure*',
          'free roms', 'rom download', 'roms download', 'download roms', 'download rom', '*warez*', '*keygen*',
          '*nulled*', 'cracked apk', 'crack download', '*thepiratebay*', '*1337x*', '*rarbg*',
          '*token grabber*', '*token logger*', '*stealer*', 'rat download', 'ddos for hire', 'ip booter',
          'booter service', '*stresser*', 'carding', 'cvv dump*', 'sell cvv', 'buy cvv', '*fullz*',
          'buy cocaine', 'buy weed', 'sell weed', 'buy meth', 'buy fentanyl', 'fake id', 'fake passport',
      ], allow_list: [] },
      actions: [block('Piracy, malware and illegal trade are not allowed here (see #rules).'), alert] },

    { name: 'Scams and phishing', event_type: 1, trigger_type: 1,
      trigger_metadata: { keyword_filter: [
          'free nitro', 'nitro giveaway', 'claim your nitro', 'claim nitro', '*discord-gift*', '*discordgift*',
          '*dlscord*', '*discrod*', '*disocrd*', '*discorcl*', '*steamcommunlty*', '*steancommunity*',
          '*stearncommunity*', 'steam gift card', 'double your crypto', 'crypto doubling', 'airdrop claim',
          'claim your airdrop', 'dm me to earn', 'investment opportunity',
      ], allow_list: [] },
      actions: [block('Links and offers like this are a common scam, so the message was blocked.'), alert, timeout(3600)] },

    { name: 'Secrets and personal data', event_type: 1, trigger_type: 1,
      trigger_metadata: { regex_patterns: [
          '[MNO][A-Za-z0-9_-]{23,25}\\.[A-Za-z0-9_-]{6}\\.[A-Za-z0-9_-]{27,38}',            // Discord bot token
          '(sk-(ant-)?[A-Za-z0-9_-]{20,}|gsk_[A-Za-z0-9]{20,}|xai-[A-Za-z0-9]{20,})',       // OpenAI / Anthropic / Groq / xAI keys
          'AIza[0-9A-Za-z_-]{35}',                                                         // Google API key
          '(ghp|gho|ghs|ghu|github_pat)_[A-Za-z0-9_]{20,}',                                // GitHub token
          '-----BEGIN [A-Z ]*PRIVATE KEY-----',
          '(?i)(wi-?fi|wlan|psk|password|passwd|pwd)\\s*[:=]\\s*\\S{6,}',                   // "password: hunter22"
          '\\b(4[0-9]{3}|5[1-5][0-9]{2})[ -]?[0-9]{4}[ -]?[0-9]{4}[ -]?[0-9]{4}\\b',         // Visa / Mastercard
          '\\b[A-Z]{2}[0-9]{2} ?[A-Z0-9]{4}( ?[0-9]{4}){2,6}',                                // IBAN (needs digit groups)
      ], keyword_filter: [], allow_list: [] },
      actions: [block('That looked like a password, key or card/bank number. Never post secrets: the message was blocked.'), alert] },

    { name: 'Mention spam', event_type: 1, trigger_type: 5,
      trigger_metadata: { mention_total_limit: 5, mention_raid_protection_enabled: true },
      actions: [block('Too many mentions in one message.'), alert, timeout(600)] },

    { name: 'Suspected spam', event_type: 1, trigger_type: 3, trigger_metadata: {},
      actions: [block('This looked like spam, so it was blocked.'), alert] },
];

const existing = await rest('GET', `/guilds/${GUILD}/auto-moderation/rules`);
for (const r of RULES) {
    const body = { ...r, enabled: true, exempt_roles: [], exempt_channels: [modlog.id] };
    const old = existing.find(x => x.name === r.name);
    if (old) { const { trigger_type, ...patch } = body; await rest('PATCH', `/guilds/${GUILD}/auto-moderation/rules/${old.id}`, patch); console.log('updated', r.name); }
    else { await rest('POST', `/guilds/${GUILD}/auto-moderation/rules`, body); console.log('created', r.name); }
}
console.log('done: AutoMod rules', RULES.length, '· #mod-log', modlog.id);
