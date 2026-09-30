<#
  sd-sync.ps1 - copia SICURA del payload di sistema NucleoOS sulla SD del Cardputer.

  Sorgente : deploy/sd/  (asset statici: apps, www, system/registry, pack ANIMA)
  Target   : la radice della SD (es. H:\)

  VOICE (integral system part, always on board):
   - the Vosk dictation models (apps/anima/www/vosk/models, split parts) travel with the deploy/sd payload;
   - the TTS clip bank (data/tts, ~800 MB) is too big for deploy/sd -> copied separately from deploy/sd-safe.
   Both without /MIR: only added/updated, never deleted (and firmware nucleo_fs_is_protected
   also prevents on-device deletion).

  GARANZIE:
   - NON cancella MAI niente sul target (nessun /MIR /PURGE): aggiunge/aggiorna soltanto.
   - PROTEGGE lo stato del device anche se per errore finisse nel payload:
       data\anima\teacher.json      (chiave Groq / config online)
       data\anima\learned\*         (card imparate + .vec)
       data\anima\telemetry.ndjson, session.txt, .httptrace
       system\config\*              (impostazioni utente create a runtime)
       config\*, backups\, journal\
   - Si rifiuta di scrivere se il target non sembra una SD NucleoOS (manca system\ o data\),
     a meno di -Force, cosi' non sovrascrivi il disco sbagliato.

  USO:
     powershell -File tools\sd-sync.ps1 -Target H:\
     powershell -File tools\sd-sync.ps1 -Target H:\ -WhatIf      # anteprima, non scrive
#>
[CmdletBinding(SupportsShouldProcess=$true)]
param(
  [Parameter(Mandatory=$true)] [string]$Target,
  [switch]$Force
)

$ErrorActionPreference = 'Stop'
$src = Join-Path $PSScriptRoot '..\deploy\sd' | Resolve-Path | Select-Object -ExpandProperty Path

if (-not (Test-Path $Target)) { throw "Target '$Target' non trovato. Inserisci la SD e controlla la lettera di unita'." }

# Sanity: e' davvero una SD NucleoOS?
$looksLikeSd = (Test-Path (Join-Path $Target 'system')) -or (Test-Path (Join-Path $Target '.deploy-manifest.json'))
if (-not $looksLikeSd -and -not $Force) {
  throw "Il target '$Target' non sembra una SD NucleoOS (manca system\ o .deploy-manifest.json). Usa -Force se sei sicuro."
}

# File/dir che NON devono mai essere toccati sul device.
# NB: data\anima\learned NON e' piu' escluso in blocco — i SEED firmware-pinned facets.<lang>.jsonl
# (read-only sul device, devono combaciare byte-per-byte con VKL_FACETS_* nel .bin) DEVONO arrivare.
# I file SCRITTI dal device dentro learned/ sono invece protetti per NOME qui sotto: la cache online
# (it.jsonl/en.jsonl + *.vec), le triple KGE runtime (mind.*.jsonl) e il ledger di evoluzione
# (knowledge.ledger.jsonl, occ/subclass.jsonl). Aggiunto sessions.json (cronologia chat reale).
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
if ($WhatIfPreference) { $flags += '/L' }   # /L = solo lista, non copia

$mode = 'COPIA'
if ($WhatIfPreference) { $mode = 'ANTEPRIMA (nessuna scrittura)' }
Write-Host "Sorgente : $src"
Write-Host "Target   : $Target"
Write-Host "Modalita : $mode"
Write-Host ("Protetti : {0}" -f ($xf -join ', '))
Write-Host ''

& robocopy "$src" "$Target" *.* @flags /XF @xf /XD @xd
$rc = $LASTEXITCODE
# robocopy: 0-7 = successo (8+ = errore reale)
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
