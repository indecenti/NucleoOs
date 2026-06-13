# gen_all.ps1 — pipeline completa voce: per ogni lingua GENERA (Edge-TTS) -> IMPACCHETTA (index.bin +
# clips.pcm), TUTTO SUL DISCO DEL PC (veloce, nessuna scrittura sulla SD). La copia su H: e' uno step
# FINALE ESPLICITO che parte solo con -CopyToSd e solo DOPO che tutte le lingue sono pronte.
# Resumable (--skip-existing): se Edge throttla o si interrompe, rilancia e riprende. IT prima, poi EN.
# I .wav di staging restano sul PC (non sul device).
#   .\gen_all.ps1                 # genera + impacchetta su disco PC, NON tocca la SD
#   .\gen_all.ps1 -CopyToSd       # come sopra, e SOLO ALLA FINE copia index.bin+clips.pcm su H:
param([int]$Conc = 10, [switch]$CopyToSd)
$ErrorActionPreference = 'Continue'
$py = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $py) { $py = (Get-Command py -ErrorAction SilentlyContinue).Source }
if (-not $py) { Write-Host 'python non trovato'; exit 1 }

$root    = 'G:\Nucleo'
$tt      = Join-Path $root 'tools\nucleo-tts'
$staging = Join-Path $tt '_wav'                                  # .wav (solo PC)
$final   = Join-Path $root 'deploy\sd-safe\data\tts'            # index.bin + clips.pcm (progetto, disco PC)
$hroot   = 'H:\data\tts'                                         # SD (toccata SOLO con -CopyToSd, alla fine)

# GENERA + IMPACCHETTA una lingua, interamente su disco PC. Ritorna $true se il pack e' pronto.
function GenPack($lang) {
  Write-Host "===== [$lang] GENERAZIONE (Edge-TTS) -> disco PC ====="
  # --no-dict: SOLO il parlato reale di ANIMA (mandatory + lexicon + lexicon.wf + freq). Senza, gen_edge
  # includerebbe il dizionario intero (~77k clip, blob ~2.9GB) -> violerebbe il vincolo "blob compatto".
  & $py (Join-Path $tt 'gen_edge.py') --langs $lang --no-dict --skip-existing --conc $Conc --out $staging
  Write-Host "===== [$lang] IMPACCHETTAMENTO -> disco PC ====="
  & $py (Join-Path $tt 'build_index.py') --in (Join-Path $staging $lang) --out (Join-Path $final $lang)
  if (-not (Test-Path (Join-Path $final "$lang\index.bin"))) { Write-Host "[$lang] PACK FALLITO"; return $false }
  return $true
}

# Copia su SD (H:) il pack gia' pronto di una lingua. Chiamata SOLO nello step finale, con -CopyToSd.
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
