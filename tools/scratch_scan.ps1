$ips = @(104, 107, 121, 123, 124, 132, 137, 141, 150, 151, 158, 162, 168, 174, 178, 179, 188, 202, 204, 218, 221, 230, 233, 240, 245, 248)
$found = $null
foreach ($num in $ips) {
    $ip = "192.168.0.$num"
    try {
        $st = Invoke-RestMethod -Uri "http://$ip/api/status" -TimeoutSec 1 -UseBasicParsing
        if ($st.os -eq "NucleoOS") {
            Write-Host "Cardputer trovato a: $ip" -ForegroundColor Green
            $found = $ip
            break
        }
    } catch {}
}
if ($found) {
    # Salva il nuovo IP in release.local.json in modo da aggiornare la configurazione locale!
    $cfg = @{ host = $found; pin = "689614" } | ConvertTo-Json
    $cfg | Out-File "tools\release.local.json" -Encoding utf8
    Write-Host "release.local.json aggiornato con successo." -ForegroundColor Green
} else {
    Write-Error "Impossibile trovare il Cardputer nella rete locale."
}
