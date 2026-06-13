<#
  push-wg-to-sd.ps1 - copia i 5 corti Wallace & Gromit (PINNED, mai da eliminare) sulla SD del Cardputer.

  Sorgente : deploy\sd\data\Videos  (copia canonica nel progetto)
  Target   : <Drive>\data\Videos     (default H:)

  - NON cancella niente: aggiunge/sovrascrive solo i 5 file pinned.
  - Si rifiuta se il drive non esiste (inserisci la SD e ricontrolla la lettera).

  USO:  powershell -ExecutionPolicy Bypass -File tools\nfv\push-wg-to-sd.ps1 -Drive H:
#>
param([string]$Drive = 'H:')
$ErrorActionPreference = 'Stop'

# I 5 corti pinned: stessi nomi protetti dalla keep-list di deploy.ps1.
$PINNED = @(
  'Wallace e Gromit - Una Fantastica Gita',              # 1989
  'Wallace e Gromit - I Pantaloni Sbagliati',            # 1993
  'Wallace e Gromit - Una Tosatura Perfetta',            # 1995
  'Wallace e Gromit - Cracking Contraptions',            # 2002
  'Wallace e Gromit - Il Mistero dei 12 Fornai Assassinati' # 2008
)

$src = Join-Path $PSScriptRoot '..\..\deploy\sd\data\Videos' | Resolve-Path | Select-Object -ExpandProperty Path
$root = $Drive.TrimEnd('\') + '\'
if (-not (Test-Path $root)) { throw "Drive '$root' non trovato. Inserisci la SD del Cardputer e controlla la lettera di unita'." }
$dst = Join-Path $root 'data\Videos'
if (-not (Test-Path $dst)) { New-Item -ItemType Directory -Force -Path $dst | Out-Null }

$n = 0
foreach ($base in $PINNED) {
  foreach ($ext in '.nfv', '.mp3') {
    $f = Join-Path $src ($base + $ext)
    if (-not (Test-Path $f)) { throw "manca la sorgente: $f (rilancia la conversione)" }
    Copy-Item -LiteralPath $f -Destination (Join-Path $dst ($base + $ext)) -Force
    $n++
  }
}
Write-Host ("OK - $n file copiati in $dst (5 corti Wallace & Gromit, pinned).")
