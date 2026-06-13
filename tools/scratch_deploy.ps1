$online = $false
for ($i = 0; $i -lt 30; $i++) {
    try {
        $st = Invoke-RestMethod "http://192.168.0.166/api/status" -TimeoutSec 2 -UseBasicParsing
        if ($st.os) { $online = $true; break }
    } catch {}
    Start-Sleep -Seconds 1
}
if (-not $online) {
    Write-Error "Dispositivo offline"
    exit 1
}
powershell -ExecutionPolicy Bypass -File tools\release.ps1 -SdOnly -SkipBuild
