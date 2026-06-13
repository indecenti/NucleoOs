# Arricchisce il set MIRATO con le parole utili frequenti (freq.<lang>.txt), STESSA voce Elsa/Aria,
# senza rigenerare le 307/253 gia' fatte (--skip-existing). Poi impacchetta e copia su H:. Conclude.
param([int]$Conc = 8)
$ErrorActionPreference = 'Continue'
$py = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $py) { $py = (Get-Command py -ErrorAction SilentlyContinue).Source }

$tt      = 'G:\Nucleo\tools\nucleo-tts'
$staging = Join-Path $tt '_wav'
$final   = 'G:\Nucleo\deploy\sd-safe\data\tts'
$h       = 'H:\data\tts'

$t0 = Get-Date
Write-Host '===== GENERAZIONE parole utili (stessa voce, salta le gia fatte) ====='
& $py (Join-Path $tt 'gen_edge.py') --langs it,en --no-dict --skip-existing --conc $Conc --out $staging

foreach ($l in @('it','en')) {
  Write-Host "===== [$l] IMPACCHETTAMENTO ====="
  & $py (Join-Path $tt 'build_index.py') --in (Join-Path $staging $l) --out (Join-Path $final $l)
  if ((Test-Path 'H:\') -and (Test-Path (Join-Path $final "$l\index.bin"))) {
    Write-Host "===== [$l] COPIA su H: ====="
    robocopy (Join-Path $final $l) (Join-Path $h $l) index.bin clips.pcm /NJH /NJS /NDL /NP /R:3 /W:5 | Out-Null
    Write-Host "[$l] robocopy exit $LASTEXITCODE (0-7 = OK)"
  }
}

Write-Host ('===== FATTO in {0:n0} min =====' -f ((Get-Date) - $t0).TotalMinutes)
foreach ($l in @('it','en')) {
  $pcm = Join-Path $final "$l\clips.pcm"; $idx = Join-Path $final "$l\index.bin"
  if (Test-Path $pcm) {
    "[$l] clips.pcm {0:n0} MB  index {1:n0} KB  wav {2}" -f ((Get-Item $pcm).Length/1MB), ((Get-Item $idx).Length/1KB), (Get-ChildItem (Join-Path $staging $l)).Count
  }
}
$onH = @(); foreach ($l in @('it','en')) { if (Test-Path (Join-Path $h "$l\index.bin")) { $onH += $l } }
Write-Host ("Su H: presenti: " + ($onH -join ', '))
