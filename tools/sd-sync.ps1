<#
  sd-sync.ps1 - SAFE copy of the NucleoOS system payload to the Cardputer SD.

  Source : deploy/sd/  (static assets: apps, www, system/registry, ANIMA packs)
  Target : the SD root (e.g. H:\)

  VOICE (integral system part, always on board):
   - the Vosk dictation models (apps/anima/www/vosk/models, split parts) travel with the deploy/sd payload;
   - the TTS clip bank (data/tts, ~800 MB) is too big for deploy/sd -> copied separately from deploy/sd-safe.
   Both without /MIR: only added/updated, never deleted (and firmware nucleo_fs_is_protected
   also prevents on-device deletion).

  GUARANTEES:
   - NEVER deletes anything on the target (no /MIR /PURGE): only adds/updates. The one exception is a
     stale .gz twin the payload no longer ships (it would be served instead of the new file).
   - PROTECTS the device / user state even if it ended up in the payload by mistake: everything
     tools/lib/sd-policy.json classifies as state (API key, learned cards, settings, keys, sessions,
     logs, the user's documents...) — the same table every SD tool uses.
   - MERGES system\registry\apps.json (keeps the user's Agent-published apps), never overwrites it.
   - Refuses to write if the target does not look like a NucleoOS SD (missing system\ or data\),
     unless -Force is given, so you do not overwrite the wrong disk.

  USAGE:
     powershell -File tools\sd-sync.ps1 -Target H:\
     powershell -File tools\sd-sync.ps1 -Target H:\ -WhatIf      # preview, does not write
#>
[CmdletBinding(SupportsShouldProcess=$true)]
param(
  [Parameter(Mandatory=$true)] [string]$Target,
  [switch]$Force
)

$ErrorActionPreference = 'Stop'
$src = Join-Path $PSScriptRoot '..\deploy\sd' | Resolve-Path | Select-Object -ExpandProperty Path

if (-not (Test-Path $Target)) { throw "Target '$Target' non trovato. Inserisci la SD e controlla la lettera di unita'." }

# Sanity: is this really a NucleoOS SD?
$looksLikeSd = (Test-Path (Join-Path $Target 'system')) -or (Test-Path (Join-Path $Target '.deploy-manifest.json'))
if (-not $looksLikeSd -and -not $Force) {
  throw "Il target '$Target' non sembra una SD NucleoOS (manca system\ o .deploy-manifest.json). Usa -Force se sei sicuro."
}

# Files that must NEVER be written on the card: every source file that tools/lib/sd-policy.json classifies as
# device/user state (the same table deploy.ps1, push-ota, sd-net-sync and sd_deploy.py use), excluded by its
# FULL source path — robocopy's name-based /XF used to block e.g. system/registry/settings.json (a system
# file) just because a state file elsewhere is also called settings.json. Plus system/registry/apps.json,
# which is MERGED afterwards (see below): the card's registry also lists the user's own Agent apps.
. (Join-Path $PSScriptRoot 'lib/sd-policy.ps1')
$xf = @(Join-Path $src 'system\registry\apps.json')
Get-ChildItem -LiteralPath $src -Recurse -File | ForEach-Object {
  $rel = ($_.FullName.Substring($src.Length).TrimStart('\', '/')) -replace '\\', '/'
  if ($rel -eq '.deploy-manifest.json' -or (Is-DeviceState $rel)) { $xf += $_.FullName }
}

$flags = @('/E','/FFT','/R:1','/W:1','/NJH','/NJS','/NDL','/NP')
if ($WhatIfPreference) { $flags += '/L' }   # /L = list only, does not copy

$mode = 'COPIA'
if ($WhatIfPreference) { $mode = 'ANTEPRIMA (nessuna scrittura)' }
Write-Host "Sorgente : $src"
Write-Host "Target   : $Target"
Write-Host "Modalita : $mode"
Write-Host ("Protected: {0} file(s) of device/user state (tools/lib/sd-policy.json) + apps.json (merged)" -f ($xf.Count - 1))
Write-Host ''

# deploy/sd is copied AS IS: warn when it is stale vs the sources (tools/deploy.ps1 rebuilds it).
$nodeExe = Get-Command node -ErrorAction SilentlyContinue
if ($nodeExe) {
  $drift = & $nodeExe.Source (Join-Path $PSScriptRoot 'staging-check.mjs')
  if ($LASTEXITCODE -eq 1) { Write-Warning ("deploy/sd is STALE: " + ($drift | Select-Object -First 1) + " - run tools\deploy.ps1 first") }
}

& robocopy "$src" "$Target" *.* @flags /XF @xf
$rc = $LASTEXITCODE
# robocopy: 0-7 = success (8+ = real error)
if ($rc -ge 8) { throw "robocopy ha riportato un errore (exit $rc)." }

# Stale .gz twins — the one kind of file removed from the card. The device serves "<file>.gz" instead of
# "<file>" in /www/shell and /apps/<id>/www, so a twin the payload does not ship, next to a raw file it does,
# would shadow the new file. Same rule as the firmware (nucleo_fsapi fstwin.c) and deploy.ps1; the regex is
# held to tools/lib/twin-scope-vectors.json by tools/anima-host/fstwin-check.mjs.
$twins = 0
Get-ChildItem -LiteralPath $src -Recurse -File | ForEach-Object {
  $rel = ($_.FullName.Substring($src.Length).TrimStart('\', '/')) -replace '\\', '/'
  if ($rel -match '^(www/shell|apps/[^/]+/www)/.' -and $rel -notmatch '\.gz$' -and -not (Test-Path -LiteralPath "$($_.FullName).gz")) {
    $g = Join-Path $Target ("$rel.gz" -replace '/', '\')
    if (Test-Path -LiteralPath $g -PathType Leaf) {
      if (-not $WhatIfPreference) { Remove-Item -LiteralPath $g -Force }
      $script:twins++
    }
  }
}
if ($twins) { Write-Host "Stale .gz twins removed: $twins" }

# Registry: merge the staged system/registry/apps.json into the card's copy (tools/lib/registry-merge.mjs):
# the release is authoritative for bundled apps, the user's Agent apps (created_by "agent") are kept.
$regSrc = Join-Path $src 'system/registry/apps.json'
$regDst = Join-Path $Target 'system/registry/apps.json'
if (Test-Path $regSrc) {
  $node = Get-Command node -ErrorAction SilentlyContinue
  if (-not $node) {
    Write-Warning "node not found: system/registry/apps.json NOT updated (it must be merged, never copied over)."
  } else {
    $merge = Join-Path $PSScriptRoot 'lib/registry-merge.mjs'
    $out = if ($WhatIfPreference) { [IO.Path]::GetTempFileName() } else { $regDst }
    if (-not $WhatIfPreference) { New-Item -ItemType Directory -Force (Split-Path $regDst) | Out-Null }
    $dev = if (Test-Path $regDst) { $regDst } else { '-' }
    $res = & $node.Source $merge $regSrc $dev $out
    if ($LASTEXITCODE -ne 0) { throw "registry merge failed (exit $LASTEXITCODE)" }
    Write-Host "Registry  : $res"
    if ($WhatIfPreference) { Remove-Item $out -ErrorAction SilentlyContinue }
  }
}

# TTS voice: the clip bank (data/tts) is not in deploy/sd (too big for the repo) ->
# copied directly from deploy/sd-safe. Same deal: no /MIR, only add/update.
$ttsSafe = Join-Path $PSScriptRoot '..\deploy\sd-safe\data\tts'
if (Test-Path (Join-Path $ttsSafe 'it\clips.pcm')) {
  $ttsSrc = (Resolve-Path $ttsSafe).Path
  Write-Host ''
  Write-Host "TTS voice : $ttsSrc -> $Target (data\tts)"
  & robocopy "$ttsSrc" (Join-Path $Target 'data\tts') *.* @flags
  if ($LASTEXITCODE -ge 8) { throw "robocopy of the TTS voice reported an error (exit $LASTEXITCODE)." }
} else {
  Write-Warning "TTS voice missing in deploy/sd-safe/data/tts - fetch it with: node oversized-assets/rejoin.mjs tts-it-clips tts-en-clips"
}

Write-Host ''
Write-Host "OK - safe copy complete (robocopy exit $rc; 0-7 = success). Voice on board, device state preserved."
exit 0
