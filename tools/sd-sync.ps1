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
   - NEVER deletes anything on the target (no /MIR /PURGE): only adds/updates.
   - PROTECTS the device state even if it ended up in the payload by mistake:
       data\anima\teacher.json      (Groq key / online config)
       data\anima\learned\*         (learned cards + .vec)
       data\anima\telemetry.ndjson, session.txt, .httptrace
       system\config\*              (user settings created at runtime)
       config\*, backups\, journal\
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

# Files/dirs that must NEVER be touched on the device.
# NB: data\anima\learned is no longer excluded as a whole — the firmware-pinned SEED facets.<lang>.jsonl
# (read-only on the device, must match byte-for-byte VKL_FACETS_* in the .bin) MUST get through.
# The files WRITTEN by the device inside learned/ are instead protected by NAME below: the online cache
# (it.jsonl/en.jsonl + *.vec), the runtime KGE triples (mind.*.jsonl) and the evolution ledger
# (knowledge.ledger.jsonl, occ/subclass.jsonl). Added sessions.json (real chat history).
$xf = @('teacher.json','telemetry.ndjson','session.txt','sessions.json','.httptrace','*.httptrace','*.vec','auth.json','volume.json','settings.json','workspace.json',
        'it.jsonl','en.jsonl','mind.it.jsonl','mind.en.jsonl','knowledge.ledger.jsonl','occ.jsonl','subclass.jsonl')
$xd = @(
  (Join-Path $Target 'system\config'),
  (Join-Path $Target 'system\keys'),
  (Join-Path $Target 'system\sessions'),
  (Join-Path $Target 'system\log'),
  (Join-Path $Target 'system\logs'),
  (Join-Path $Target 'config'),
  (Join-Path $Target 'backups'),
  (Join-Path $Target 'journal')
)

$flags = @('/E','/FFT','/R:1','/W:1','/NJH','/NJS','/NDL','/NP')
if ($WhatIfPreference) { $flags += '/L' }   # /L = list only, does not copy

$mode = 'COPIA'
if ($WhatIfPreference) { $mode = 'ANTEPRIMA (nessuna scrittura)' }
Write-Host "Sorgente : $src"
Write-Host "Target   : $Target"
Write-Host "Modalita : $mode"
Write-Host ("Protetti : {0}" -f ($xf -join ', '))
Write-Host ''

& robocopy "$src" "$Target" *.* @flags /XF @xf /XD @xd
$rc = $LASTEXITCODE
# robocopy: 0-7 = success (8+ = real error)
if ($rc -ge 8) { throw "robocopy ha riportato un errore (exit $rc)." }

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
