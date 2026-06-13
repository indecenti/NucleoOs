param (
    [Parameter(Mandatory=$true)]
    [string]$IP,
    
    [Parameter(Mandatory=$true)]
    [string]$PIN,
    
    [Parameter(Mandatory=$true)]
    [string]$FilePath,
    
    [Parameter(Mandatory=$true)]
    [string]$DestinationPath
)

# Esempio d'uso: 
# .\upload_ota.ps1 -IP 192.168.1.50 -PIN 689614 -FilePath ..\web\shell\wallpaper.png -DestinationPath /www/shell/wallpaper.png

$pairUrl = "http://$IP/api/pair"
$uploadUrl = "http://$IP/api/fs/write?path=$DestinationPath"

Write-Host "Autenticazione in corso con il Cardputer ($IP)..."
$body = @{ pin = $PIN } | ConvertTo-Json
$session = $null

try {
    $pairResponse = Invoke-RestMethod -Uri $pairUrl -Method Post -Body $body -ContentType "application/json" -SessionVariable session
    Write-Host "Autenticazione (Pairing) riuscita. Inizio upload..."
} catch {
    Write-Host "Errore durante il pairing (PIN errato?):" -ForegroundColor Red
    Write-Host $_.Exception.Message
    exit
}

$fileBytes = [System.IO.File]::ReadAllBytes($FilePath)
Write-Host "Trasferimento di $FilePath verso $uploadUrl..."

try {
    $response = Invoke-RestMethod -Uri $uploadUrl -Method Post -Body $fileBytes -ContentType "application/octet-stream" -WebSession $session
    Write-Host "Upload completato con successo!" -ForegroundColor Green
    Write-Host "Bytes trasferiti: $($response.bytes)"
} catch {
    Write-Host "Errore durante l'upload:" -ForegroundColor Red
    Write-Host $_.Exception.Message
}
