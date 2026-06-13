$ErrorActionPreference = 'Stop'
$session = New-Object Microsoft.PowerShell.Commands.WebRequestSession
Write-Host "Pairing..."
Invoke-WebRequest "http://192.168.0.166/api/pair" -Method Post -Body (@{ pin = '689614' } | ConvertTo-Json) -ContentType 'application/json' -WebSession $session -UseBasicParsing | Out-Null
Write-Host "Uploading asr.js..."
Invoke-WebRequest "http://192.168.0.166/api/fs/write?path=/apps/dictation/www/asr.js" -Method Post -InFile "g:\Nucleo\apps\dictation\www\asr.js" -ContentType 'application/octet-stream' -WebSession $session -UseBasicParsing | Out-Null
Write-Host "Uploading asr.js.gz..."
Invoke-WebRequest "http://192.168.0.166/api/fs/write?path=/apps/dictation/www/asr.js.gz" -Method Post -InFile "g:\Nucleo\deploy\sd\apps\dictation\www\asr.js.gz" -ContentType 'application/octet-stream' -WebSession $session -UseBasicParsing | Out-Null
Write-Host "Done!"
