# NucleoOS community Discord bot

Minimal on purpose, and **free when idle**. Three scripts run once from a PC; one tiny program runs only
while someone is using it.

| Part | Runs where | What it does |
|---|---|---|
| `automod.mjs` | once, from a PC | Creates the server's **Discord AutoMod** rules (Discord's lists for profanity / sexual content / slurs, insults in IT·ES·FR·DE, piracy·malware·illegal trade, scams·phishing, leaked secrets: tokens, API keys, passwords, cards, IBANs; mention spam, spam), a Team-only `#mod-log`, explicit-media scanning, Medium verification. All of it then runs **on Discord's side**: no process of ours reads messages. |
| `harden.mjs` | once, from a PC | Least privilege for members (no `@everyone` pings, no emoji/event creation, no outside apps, no private threads), moderator powers for the Team, slowmode, forums that require a tag and allow one new post per 2 minutes, a permanent invite. The bot can only grant permissions it holds and edit roles below its own: it prints what the owner must tick by hand. |
| `register.mjs` | from a PC, after each command change | Registers `/ask` and `/guide` (server + DMs with the bot), sets the bot's profile text, points Discord at the interactions endpoint. The only script that needs the bot token. |
| `bot.mjs` | VM, on demand | Answers `/ask <question>` and `/guide <topic>` from `kb.json`. Nothing else. |

## How "free when idle" works

Discord **POSTs** every slash command to `https://nucleoos.indexhub.it/discord/interactions` (nginx →
`127.0.0.1:8009`). systemd holds that port (`deploy/nucleoos-bot.socket`) and starts `bot.mjs` on the first
request; the bot exits after 5 idle minutes. At rest: **no process, 0 RAM, 0 CPU**. Running: ~15 MB.
Cold start ≈ 1.7 s on the VM (Discord allows 3 s), then ~40 ms per answer.

## Why it can't be abused

- **No token on the server**: the VM holds only the application's *public* key and verifies Discord's
  Ed25519 signature (and a 5-minute timestamp window) on every request; anything else gets 401.
- No dependencies (Node ≥ 22 built-ins), no gateway, reads no messages.
- Replies come **only from `kb.json`** — user text is never echoed, mentions are never pinged.
- Works only in our guild and in DMs with the bot; turn **Public Bot off** in the Developer Portal.
- 15 s per-user cooldown, 30 answers/minute, 200-char questions, 16 KB request cap, nginx rate limit.
- systemd caps (`deploy/nucleoos-bot.service`): 64 MB RAM hard limit, 10 % CPU, no swap, read-only system,
  no privileges.
- Off-topic questions get "I couldn't find that in the docs" — better silence than a wrong answer.

## Editing answers

`kb.json`: every entry has `title`, `keywords` (English + Italian words people type), `answer` (Discord
markdown, < 1500 chars), `guide` (topic for `/guide`) and `src` (the doc it summarizes — keep them in sync
with `docs/user-guide.md`). Add a case to `search.test.mjs` for each new entry, then:

```bash
npm run discord:test
```

and redeploy with `deploy/deploy.ps1 -SshHost <user@vm> -Key <ssh key>` (add `-NodeTarball` on a fresh VM:
the official linux-x64 `.tar.xz`, SHA-256 checked against nodejs.org's `SHASUMS256.txt`). The nginx
location lives in the website repo (`deploy/nucleoos.conf`).
