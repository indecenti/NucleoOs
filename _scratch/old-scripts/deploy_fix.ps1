$c = Get-Content web/shell/sw.js -Raw
if ($c -match 'nucleo-shell-v(\d+)') {
    $old = $matches[1]
    $new = [int]$old + 1
    $c = $c -replace "nucleo-shell-v$old", "nucleo-shell-v$new"
    Set-Content web/shell/sw.js $c
    Write-Host "Bumped sw.js to v$new"
}
Copy-Item web/shell/shell.js, web/shell/sw.js deploy/sd-safe/www/shell/ -Force
Copy-Item web/shell/shell.js, web/shell/sw.js deploy/sd/www/shell/ -Force
Copy-Item apps/help/www/index.html deploy/sd-safe/apps/help/www/index.html -Force
Copy-Item apps/help/www/index.html deploy/sd/apps/help/www/index.html -Force

node tools/gzip-assets.mjs deploy/sd-safe/www deploy/sd-safe/apps deploy/sd/www deploy/sd/apps
.\tools\sd-sync.ps1 -Target H:\
python tools\serial_boot.py COM3 10

Copy-Item apps/spreadsheet/www/index.html deploy/sd-safe/apps/spreadsheet/www/index.html -Force
Copy-Item apps/spreadsheet/www/index.html deploy/sd/apps/spreadsheet/www/index.html -Force
