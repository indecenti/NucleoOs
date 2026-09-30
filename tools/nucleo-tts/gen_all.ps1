# gen_all.ps1 — full voice pipeline: for each language GENERATE (Edge-TTS) -> PACK (index.bin +
# clips.pcm), ALL ON THE PC DISK (fast, no writes to the SD). Copying to H: is an EXPLICIT
# FINAL step that runs only with -CopyToSd and only AFTER all the languages are ready.
# Resumable (--skip-existing): if Edge throttles or is interrupted, rerun and it resumes. IT first, then EN.
# The staging .wav files stay on the PC (not on the device).
#   .\gen_all.ps1                 # generate + pack on the PC disk, does NOT touch the SD
#   .\gen_all.ps1 -CopyToSd       # as above, and ONLY AT THE END copies index.bin+clips.pcm to H:
param([int]$Conc = 10, [switch]$CopyToSd)
$ErrorActionPreference = 'Continue'
$py = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $py) { $py = (Get-Command py -ErrorAction SilentlyContinue).Source }
if (-not $py) { Write-Host 'python non trovato'; exit 1 }

$root    = 'G:\Nucleo'
$tt      = Join-Path $root 'tools\nucleo-tts'
$staging = Join-Path $tt '_wav'                                  # .wav (PC only)
$final   = Join-Path $root 'deploy\sd-safe\data\tts'            # index.bin + clips.pcm (project, PC disk)
$hroot   = 'H:\data\tts'                                         # SD (touched ONLY with -CopyToSd, at the end)

# GENERATE + PACK one language, entirely on the PC disk. Returns $true if the pack is ready.
function GenPack($lang) {
  Write-Host "===== [$lang] GENERAZIONE (Edge-TTS) -> disco PC ====="
  # --no-dict: ONLY the real speech of ANIMA (mandatory + lexicon + lexicon.wf + freq). Without it, gen_edge
  # would include the whole dictionary (~77k clips, ~2.9GB blob) -> violating the "compact blob" constraint.
  & $py (Join-Path $tt 'gen_edge.py') --langs $lang --no-dict --skip-existing --conc $Conc --out $staging
  Write-Host "===== [$lang] IMPACCHETTAMENTO -> disco PC ====="
  & $py (Join-Path $tt 'build_index.py') --in (Join-Path $staging $lang) --out (Join-Path $final $lang)
  if (-not (Test-Path (Join-Path $final "$lang\index.bin"))) { Write-Host "[$lang] PACK FALLITO"; return $false }
  return $true
}

# Copy the already-built pack of a language to the SD (H:). Called ONLY in the final step, with -CopyToSd.
function CopyToSdLang($lang) {
  $pack = Join-Path $final $lang
  if (-not (Test-Path (Join-Path $pack 'index.bin'))) { Write-Host "[$lang] pack assente, salto"; return }
  robocopy $pack (Join-Path $hroot $lang) index.bin clips.pcm /NJH /NJS /NDL /NP /R:2 /W:2 | Out-Null
  Write-Host "[$lang] robocopy exit $LASTEXITCODE (0-7 = OK)"
}

$t0 = Get-Date
$ok = @()
if (GenPack 'it') { $ok += 'it' }
if (GenPack 'en') { $ok += 'en' }
Write-Host ("===== GENERAZIONE+PACK FATTO in {0:n0} min (su disco PC, deploy/sd-safe/data/tts) =====" -f ((Get-Date) - $t0).TotalMinutes)

if ($CopyToSd) {
  if (Test-Path 'H:\') {
    Write-Host "===== COPIA FINALE su H: (SD) ====="
    foreach ($l in $ok) { CopyToSdLang $l }
  } else { Write-Host 'H: non montata: salto la copia (i file restano in deploy/sd-safe)' }
} else {
  Write-Host 'SD NON toccata (come richiesto). Per copiare su SD quando hai finito:  .\gen_all.ps1 -CopyToSd'
}
