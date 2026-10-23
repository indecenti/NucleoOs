#!/usr/bin/env node
// Server hardening for the NucleoOS community (re-runnable). Least privilege for members, real moderation
// powers for the Team, slowmode, forum hygiene, a permanent invite. Env: DISCORD_TOKEN, DISCORD_GUILD_ID.
const TOKEN = process.env.DISCORD_TOKEN, GUILD = process.env.DISCORD_GUILD_ID;
if (!TOKEN || !GUILD) { console.error('DISCORD_TOKEN and DISCORD_GUILD_ID are required'); process.exit(1); }

async function rest(method, path, body) {
    const r = await fetch('https://discord.com/api/v10' + path, { method, body: body && JSON.stringify(body),
        headers: { Authorization: `Bot ${TOKEN}`, 'Content-Type': 'application/json', 'User-Agent': 'DiscordBot (nucleoos-bot, 1.0)' } });
    if (r.status === 429) { await new Promise(s => setTimeout(s, 2000)); return rest(method, path, body); }
    if (!r.ok) throw new Error(`${method} ${path} -> ${r.status} ${await r.text()}`);
    return r.status === 204 ? null : r.json();
}
const bit = n => 1n << BigInt(n);
const P = {
    CreateInstantInvite: bit(0), KickMembers: bit(1), BanMembers: bit(2), AddReactions: bit(6), ViewAuditLog: bit(7),
    ViewChannel: bit(10), SendMessages: bit(11), ManageMessages: bit(13), EmbedLinks: bit(14), AttachFiles: bit(15),
    ReadMessageHistory: bit(16), MentionEveryone: bit(17), UseExternalEmojis: bit(18), Connect: bit(20), Speak: bit(21),
    MuteMembers: bit(22), MoveMembers: bit(24), UseVAD: bit(25), ChangeNickname: bit(26), ManageNicknames: bit(27),
    UseApplicationCommands: bit(31), ManageThreads: bit(34), CreatePublicThreads: bit(35), UseExternalStickers: bit(37),
    SendMessagesInThreads: bit(38), ModerateMembers: bit(40), SendVoiceMessages: bit(46), SendPolls: bit(49),
};
const sum = list => String(list.reduce((a, k) => a | P[k], 0n));

// Members: talk, react, share, use /ask. No @everyone pings, no emoji/event creation, no outside apps, no private threads.
const MEMBER = ['CreateInstantInvite', 'ViewChannel', 'SendMessages', 'SendMessagesInThreads', 'CreatePublicThreads',
    'EmbedLinks', 'AttachFiles', 'ReadMessageHistory', 'AddReactions', 'UseExternalEmojis', 'UseExternalStickers',
    'ChangeNickname', 'UseApplicationCommands', 'Connect', 'Speak', 'UseVAD', 'SendVoiceMessages', 'SendPolls'];
// Team: moderators on top of the member set.
const TEAM = ['ManageMessages', 'ManageThreads', 'ModerateMembers', 'KickMembers', 'BanMembers', 'ViewAuditLog',
    'MentionEveryone', 'ManageNicknames', 'MuteMembers', 'MoveMembers'];

const roles = await rest('GET', `/guilds/${GUILD}/roles`);
const byName = n => roles.find(r => r.name === n);
const botRole = roles.find(r => r.managed && r.tags?.bot_id);
const botPerms = BigInt(botRole?.permissions ?? 0);
const tryRole = async (label, role, body) => {
    try { await rest('PATCH', `/guilds/${GUILD}/roles/${role.id}`, body); console.log('role ok:', label); }
    catch { console.log(`role ${label}: blocked by the role order — drag "NucleoOS Bot" to the TOP of Server Settings ▸ Roles, then re-run`); }
};
await tryRole('@everyone (least privilege)', { id: GUILD }, { permissions: sum(MEMBER) });
// A bot can only grant permissions it holds itself: the rest of the Team set is listed for the owner to tick.
const grantable = TEAM.filter(k => botPerms & P[k]);
const byHand = TEAM.filter(k => !(botPerms & P[k]));
await tryRole('Team', byName('Team'), { permissions: sum(grantable), hoist: true, mentionable: false });
await tryRole('Tester (badge only)', byName('Tester'), { permissions: '0', hoist: true, mentionable: false });
if (byHand.length) console.log('Team: tick by hand in Server Settings ▸ Roles ▸ Team:', byHand.join(', '));

// Slowmode (seconds between messages per user) and forum hygiene.
const chans = await rest('GET', `/guilds/${GUILD}/channels`);
const ch = n => chans.find(c => c.name === n);
const SLOW = { general: 5, showcase: 30, ideas: 60, 'anima-ai': 5, games: 5, 'security-lab': 10, development: 5 };
for (const [n, s] of Object.entries(SLOW)) if (ch(n)) await rest('PATCH', `/channels/${ch(n).id}`, { rate_limit_per_user: s });
for (const n of ['help', 'bug-reports']) {
    if (!ch(n)) continue;
    await rest('PATCH', `/channels/${ch(n).id}`, {
        flags: 1 << 4,                         // REQUIRE_TAG: every post picks at least one tag
        rate_limit_per_user: 120,              // one new post every 2 minutes per user
        default_thread_rate_limit_per_user: 3, // replies inside a post
        default_auto_archive_duration: 10080,  // a week
        default_sort_order: 0,                 // latest activity first
    });
}
console.log('slowmode + forum rules set');

// One permanent invite, created in #welcome (reused if it already exists).
const invites = await rest('GET', `/guilds/${GUILD}/invites`);
let inv = invites.find(i => i.channel?.name === 'welcome' && i.max_age === 0 && i.max_uses === 0);
if (!inv) inv = await rest('POST', `/channels/${ch('welcome').id}/invites`, { max_age: 0, max_uses: 0, unique: true });
console.log('invite: https://discord.gg/' + inv.code);
