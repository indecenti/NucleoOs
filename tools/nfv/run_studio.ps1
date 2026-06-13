# Lancia NFV Studio GUI (convertitore video drag-and-drop per il Cardputer).
# Installa al primo avvio le dipendenze mancanti: tkinterdnd2 (drag&drop) + pillow/numpy (engine v3).

$ErrorActionPreference = "Stop"
$pyFile = Join-Path $PSScriptRoot "studio_gui.py"

function Ensure-PyModule($import, $pip, $label) {
    python -c "import $import" 2>$null
    if (-not $?) {
        Write-Host "Installazione $label ..."
        python -m pip install $pip --quiet
    }
}

Ensure-PyModule "tkinterdnd2" "tkinterdnd2" "tkinterdnd2 (drag & drop)"
Ensure-PyModule "PIL"         "pillow"      "Pillow (engine v3)"
Ensure-PyModule "numpy"       "numpy"       "numpy (engine v3)"

# ffmpeg deve essere nel PATH (lo usa sia v2 che v3).
if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) {
    Write-Warning "ffmpeg non trovato nel PATH: la conversione non funzionera' finche' non lo installi."
}

python $pyFile @args
