<#
  push-wg-to-sd.ps1 - copies the 5 Wallace & Gromit shorts (PINNED, never to be deleted) to the Cardputer SD.

  Source : deploy\sd\data\Videos  (canonical copy in the project)
  Target : <Drive>\data\Videos     (default H:)

  - Does NOT delete anything: only adds/overwrites the 5 pinned files.
  - Refuses if the drive does not exist (insert the SD and re-check the letter).

  USAGE:  powershell -ExecutionPolicy Bypass -File tools\nfv\push-wg-to-sd.ps1 -Drive H:
#>
param([string]$Drive = 'H:')
$ErrorActionPreference = 'Stop'

# The 5 pinned shorts: same names protected by the keep-list of deploy.ps1.
$PINNED = @(
  'Wallace e Gromit - Una Fantastica Gita',              # 1989
  'Wallace e Gromit - I Pantaloni Sbagliati',            # 1993
  'Wallace e Gromit - Una Tosatura Perfetta',            # 1995
  'Wallace e Gromit - Cracking Contraptions',            # 2002
  'Wallace e Gromit - Il Mistero dei 12 Fornai Assassinati' # 2008
)

$src = Join-Path $PSScriptRoot '..\..\deploy\sd\data\Videos' | Resolve-Path | Select-Object -ExpandProperty Path
$root = $Drive.TrimEnd('\') + '\'
if (-not (Test-Path $root)) { throw "Drive '$root' not found. Insert the Cardputer's SD card and check the drive letter." }
$dst = Join-Path $root 'data\Videos'
if (-not (Test-Path $dst)) { New-Item -ItemType Directory -Force -Path $dst | Out-Null }

$n = 0
foreach ($base in $PINNED) {
  foreach ($ext in '.nfv', '.mp3') {
    $f = Join-Path $src ($base + $ext)
    if (-not (Test-Path $f)) { throw "source missing: $f (run the conversion again)" }
    Copy-Item -LiteralPath $f -Destination (Join-Path $dst ($base + $ext)) -Force
    $n++
  }
}
Write-Host ("OK - $n files copied to $dst (the 5 pinned Wallace & Gromit shorts).")
