$ErrorActionPreference = 'Stop'
$session = New-Object Microsoft.PowerShell.Commands.WebRequestSession
Write-Host "Pairing..."
Invoke-WebRequest "http://192.168.0.166/api/pair" -Method Post -Body (@{ pin = '689614' } | ConvertTo-Json) -ContentType 'application/json' -WebSession $session -UseBasicParsing | Out-Null

$files = @(
    "apps/settings/www/index.html",
    "apps/settings/www/index.html.gz",
    "apps/dictation/www/index.html",
    "apps/dictation/www/index.html.gz",
    "apps/dictation/www/asr.js",
    "apps/dictation/www/asr.js.gz",
    "apps/anima/www/voice.js",
    "apps/anima/www/voice.js.gz",
    "apps/groq-chat/www/index.html",
    "apps/groq-chat/www/index.html.gz",
    "www/shell/shell.js",
    "www/shell/shell.js.gz"
)

foreach ($f in $files) {
    Write-Host "Uploading $f..."
    # Ensure directory exists first
    $dir = Split-Path $f -Parent
    Invoke-WebRequest "http://192.168.0.166/api/fs/mkdir?path=/$dir" -Method Post -WebSession $session -UseBasicParsing -ErrorAction Ignore | Out-Null
    # Upload file
    Invoke-WebRequest "http://192.168.0.166/api/fs/write?path=/$f" -Method Post -InFile "g:\Nucleo\deploy\sd\$f" -ContentType 'application/octet-stream' -WebSession $session -UseBasicParsing | Out-Null
}
Write-Host "Done!"
