# Launch the NFV Studio GUI (drag-and-drop video converter for the Cardputer).
# On first launch, installs the missing dependencies: tkinterdnd2 (drag&drop) + pillow/numpy (engine v4).

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
Ensure-PyModule "PIL"         "pillow"      "Pillow (engine v4)"
Ensure-PyModule "numpy"       "numpy"       "numpy (engine v4)"

# ffmpeg must be on the PATH (nfv4.py drives it).
if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) {
    Write-Warning "ffmpeg non trovato nel PATH: la conversione non funzionera' finche' non lo installi."
}

python $pyFile @args
