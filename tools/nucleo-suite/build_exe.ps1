# build_exe.ps1 — compila la NucleoOS Toolkit in un .exe distribuibile.
#
# Uso (da qualsiasi cartella):
#   powershell -ExecutionPolicy Bypass -File tools\nucleo-suite\build_exe.ps1
#
# Risultato: tools\nucleo-suite\dist\NucleoSuite\NucleoSuite.exe  (cartella unica
# da zippare e distribuire). Ricordati che ffmpeg deve stare sul PATH dell'utente
# finale per il convertitore video.

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here

Write-Host "[1/3] Verifico le dipendenze..." -ForegroundColor Cyan
$deps = @("pyinstaller", "pillow", "numpy", "pyserial")
foreach ($d in $deps) {
    $shown = python -m pip show $d 2>$null
    if (-not $shown) {
        Write-Host "  installo $d" -ForegroundColor Yellow
        python -m pip install $d
    }
}
# tkinterdnd2 è opzionale (drag&drop nel converter): installalo se vuoi includerlo
python -m pip show tkinterdnd2 2>$null | Out-Null
if (-not $?) { Write-Host "  (tkinterdnd2 assente: drag&drop disattivato nel bundle)" -ForegroundColor DarkGray }

Write-Host "[2/3] Selftest del wiring..." -ForegroundColor Cyan
python launcher.py --selftest
if ($LASTEXITCODE -ne 0) { throw "Selftest fallito: script mancanti." }

Write-Host "[3/3] PyInstaller..." -ForegroundColor Cyan
python -m PyInstaller --noconfirm --clean NucleoSuite.spec

$exe = Join-Path $here "dist\NucleoSuite\NucleoSuite.exe"
if (Test-Path $exe) {
    Write-Host "`nOK -> $exe" -ForegroundColor Green
    Write-Host "Zippa la cartella dist\NucleoSuite\ per distribuirla." -ForegroundColor Green
} else {
    throw "Build completata ma NucleoSuite.exe non trovato."
}
