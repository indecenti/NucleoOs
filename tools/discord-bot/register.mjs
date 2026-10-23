#!/usr/bin/env node
// Run from a PC (needs the bot token, which never goes to the server): registers /ask and /guide in our
// guild and in DMs with the bot, sets the bot's profile text, and points Discord at the interactions
// endpoint (Discord sends a signed PING there first, so the endpoint must already be live).
// Env: DISCORD_TOKEN, DISCORD_GUILD_ID. Usage: node register.mjs [https://host/discord/interactions]
const TOKEN = process.env.DISCORD_TOKEN, GUILD = process.env.DISCORD_GUILD_ID, ENDPOINT = process.argv[2];
if (!TOKEN || !GUILD) { console.error('DISCORD_TOKEN and DISCORD_GUILD_ID are required'); process.exit(1); }
import { GUIDES } from './search.mjs';

async function rest(method, path, body) {
    const r = await fetch('https://discord.com/api/v10' + path, { method, body: body && JSON.stringify(body),
        headers: { Authorization: `Bot ${TOKEN}`, 'Content-Type': 'application/json', 'User-Agent': 'DiscordBot (nucleoos-bot, 1.0)' } });
    if (!r.ok) throw new Error(`${method} ${path} -> ${r.status} ${await r.text()}`);
    return r.status === 204 ? null : r.json();
}

const COMMANDS = [
    { name: 'ask', description: 'Ask a question about NucleoOS (install, web desktop, ANIMA, updates…)', type: 1,
      options: [{ type: 3, name: 'question', description: 'Your question', required: true, min_length: 3, max_length: 200 }] },
    { name: 'guide', description: 'Quick guides for NucleoOS', type: 1,
      options: [{ type: 3, name: 'topic', description: 'Pick a topic', required: true,
                  choices: Object.entries(GUIDES).map(([value, name]) => ({ name, value })) }] },
];

const app = await rest('GET', '/applications/@me');
await rest('PUT', `/applications/${app.id}/guilds/${GUILD}/commands`, COMMANDS);
await rest('PUT', `/applications/${app.id}/commands`, COMMANDS.map(c => ({ ...c, contexts: [1], integration_types: [0] })));
console.log('commands: guild + DM');

await rest('PATCH', '/applications/@me', {
    description: 'NucleoOS helper. Type /ask with your question (e.g. /ask how do I install it?) or /guide for quick guides. I don\'t read normal messages: for real problems use the help forum.',
    ...(ENDPOINT ? { interactions_endpoint_url: ENDPOINT } : {}),
});
console.log('profile text set', ENDPOINT ? `· endpoint ${ENDPOINT}` : '');
console.log('public key:', app.verify_key);
