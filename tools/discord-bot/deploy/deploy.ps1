# Deploy the NucleoOS Discord bot to a Linux VM over SSH (see ../README.md).
#   .\tools\discord-bot\deploy\deploy.ps1 -SshHost user@host -Key path\to\key [-NodeTarball node-v22.x-linux-x64.tar.xz]
# The host is shared and small: no package manager, no builds, bandwidth-limited copies, memory checked
# before and after. The VM gets only the application's PUBLIC key (fetched here with the DISCORD_TOKEN user
# variable, which never leaves this PC). Node is installed from the official tarball when -NodeTarball is given.
param(
    [Parameter(Mandatory)] [string] $SshHost,
    [Parameter(Mandatory)] [string] $Key,
    [string] $GuildId = '1563226728762056854',
    [string] $NodeTarball
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Split-Path -Parent $here
$ssh = { param($cmd) & ssh -i $Key -o BatchMode=yes $SshHost $cmd; if ($LASTEXITCODE) { throw "ssh failed" } }

$token = [Environment]::GetEnvironmentVariable('DISCORD_TOKEN', 'User')
if (-not $token) { throw 'DISCORD_TOKEN user variable is not set' }
$app = Invoke-RestMethod https://discord.com/api/v10/applications/@me -Headers @{ Authorization = "Bot $token"; 'User-Agent' = 'DiscordBot (nucleoos-bot, 1.0)' }
$envFile = New-TemporaryFile
[IO.File]::WriteAllText($envFile, "DISCORD_PUBLIC_KEY=$($app.verify_key)`nDISCORD_GUILD_ID=$GuildId`n")

& $ssh 'echo BEFORE; free -m | head -2; cat /proc/loadavg; rm -rf /tmp/nbot && mkdir -p /tmp/nbot'
try {
    if ($NodeTarball) { & scp -i $Key -l 4000 -q $NodeTarball "${SshHost}:/tmp/nbot/node.tar.xz" }
    & scp -i $Key -q $envFile "$src\bot.mjs" "$src\search.mjs" "$src\kb.json" "$here\nucleoos-bot.service" "$here\nucleoos-bot.socket" "${SshHost}:/tmp/nbot/"
} finally { Remove-Item $envFile -Force }

& $ssh @'
set -e
if [ -f /tmp/nbot/node.tar.xz ]; then
  sudo mkdir -p /opt/node22
  sudo nice -n 19 tar -xJf /tmp/nbot/node.tar.xz -C /opt/node22 --strip-components=1
  sudo chown -R root:root /opt/node22 && sudo restorecon -R /opt/node22
fi
id nucleobot >/dev/null 2>&1 || sudo useradd --system --no-create-home --shell /sbin/nologin nucleobot
sudo install -o root -g root -m 600 /tmp/nbot/$(ls /tmp/nbot | grep '^tmp') /etc/nucleoos-bot.env
sudo mkdir -p /opt/nucleoos-bot
sudo install -o root -g root -m 644 /tmp/nbot/bot.mjs /tmp/nbot/search.mjs /tmp/nbot/kb.json /opt/nucleoos-bot/
sudo install -o root -g root -m 644 /tmp/nbot/nucleoos-bot.service /tmp/nbot/nucleoos-bot.socket /etc/systemd/system/
sudo restorecon -R /opt/nucleoos-bot /etc/nucleoos-bot.env /etc/systemd/system/nucleoos-bot.service /etc/systemd/system/nucleoos-bot.socket
rm -rf /tmp/nbot
sudo systemctl disable nucleoos-bot.service >/dev/null 2>&1 || true   # the old always-on unit had an [Install]
sudo systemctl stop nucleoos-bot.service || true
sudo systemctl daemon-reload
sudo systemctl enable --now nucleoos-bot.socket
systemctl is-active nucleoos-bot.socket
echo AFTER; free -m | head -2; cat /proc/loadavg
'@
