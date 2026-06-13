Write-Host "Inizio scansione broadcast/ARP asincrona..."
$ping = New-Object System.Net.NetworkInformation.Ping
2..254 | ForEach-Object {
    try { $ping.SendAsync("192.168.0.$_", 150, $null) } catch {}
}
# Attende 2 secondi affinché arrivino le risposte ARP
Start-Sleep -Seconds 2

# Ora legge la nuova tabella ARP
$arpLines = arp -a | Select-String "192.168.0."
$ips = @()
foreach ($line in $arpLines) {
    if ($line -match "192\.168\.0\.(\d+)\s+") {
        $ips += [int]$Matches[1]
    }
}
$ips = $ips | Select-Object -Unique

Write-Host "Trovati $($ips.Count) IP attivi nella tabella ARP. Analizzo..."

$found = $null
foreach ($num in $ips) {
    $ip = "192.168.0.$num"
    if ($ip -eq "192.168.0.1" -or $ip -eq "192.168.0.216") { continue } # Salta router e se stessi
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
    $cfg = @{ host = $found; pin = "689614" } | ConvertTo-Json
    $cfg | Out-File "tools\release.local.json" -Encoding utf8
    Write-Host "release.local.json aggiornato con successo a $found." -ForegroundColor Green
} else {
    Write-Error "Cardputer non rilevato dopo il ripopolamento ARP."
}
