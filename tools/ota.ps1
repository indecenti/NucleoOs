# Wi-Fi firmware update (no USB): pushes the built app image to the device's /api/ota.
# The device writes it to the spare OTA slot, verifies it, sets it as boot, and reboots.
#
# /api/ota now requires PAIRING (see docs/security.md): a session cookie proves the
# client entered the 6-digit PIN shown on the Cardputer screen (Connection app -> Pair).
# Pass -Pin <code>; this script pairs first (POST /api/pair) and reuses the resulting
# nucleo_session cookie for the OTA POST. Without -Pin it probes /api/auth/status and,
# if pairing is required, tells you to read the PIN and retry.
#
# Usage:  powershell -ExecutionPolicy Bypass -File tools\ota.ps1 [-Host 192.168.0.166] [-Pin 123456]
#         (defaults to nucleo-01.local; pass -Host <ip> if mDNS doesn't resolve)
param([string]$DeviceHost = "nucleo-01.local", [string]$Pin = "")
$ErrorActionPreference = 'Stop'
$bin = Join-Path (Split-Path $PSScriptRoot -Parent) "firmware\build\nucleoos.bin"
if (-not (Test-Path $bin)) { Write-Error "Build first: $bin not found"; exit 1 }
$size = (Get-Item $bin).Length

# Pairing: reuse one WebSession so the session cookie set by /api/pair rides along on /api/ota.
$session = New-Object Microsoft.PowerShell.Commands.WebRequestSession
if ($Pin) {
    try {
        Invoke-WebRequest "http://$DeviceHost/api/pair" -Method Post -Body (@{ pin = $Pin } | ConvertTo-Json) `
            -ContentType 'application/json' -WebSession $session -TimeoutSec 15 -UseBasicParsing | Out-Null
        Write-Host "Paired with device (session cookie acquired)."
    } catch {
        Write-Error "Pairing rejected: check the PIN on the Cardputer screen. ($($_.Exception.Message))"
        exit 1
    }
} else {
    # No PIN given: ask the device whether pairing is needed before attempting the OTA.
    # (Decide OUTSIDE the try: under -ErrorActionPreference Stop a Write-Error inside it
    #  would be caught by our own catch and mistaken for an absent endpoint.)
    $st = $null
    try { $st = Invoke-RestMethod "http://$DeviceHost/api/auth/status" -TimeoutSec 15 -UseBasicParsing }
    catch { }   # /api/auth/status absent (older firmware) -> assume open, let the OTA POST surface any error.
    if ($st -and $st.required -and -not $st.paired) {
        Write-Error "This device requires pairing. Read the 6-digit PIN off the Cardputer screen (Connection app -> Pair) and re-run with:  -Pin <code>"
        exit 1
    }
}

Write-Host "OTA -> http://$DeviceHost/api/ota  ($([math]::Round($size/1KB)) KB)"
try {
    $resp = Invoke-WebRequest "http://$DeviceHost/api/ota" -Method Post -InFile $bin -ContentType 'application/octet-stream' -WebSession $session -TimeoutSec 120 -UseBasicParsing
    Write-Host "Device: $($resp.Content)"
    Write-Host "OK - device is rebooting into the new firmware (~5s)."
} catch {
    Write-Error "OTA failed: $($_.Exception.Message)"
    exit 1
}
