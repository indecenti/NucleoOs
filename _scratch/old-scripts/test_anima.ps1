$s = New-Object Microsoft.PowerShell.Commands.WebRequestSession
Invoke-WebRequest 'http://192.168.0.166/api/pair' -Method Post -Body '{"pin":"689614"}' -ContentType 'application/json' -WebSession $s -UseBasicParsing | Out-Null

function Test-Anima {
    param([string]$query, [string]$lang="it")
    Write-Host "`n=== Test: $query ==="
    $url = "http://192.168.0.166/api/anima?q=" + [uri]::EscapeDataString($query) + "&lang=$lang&mode=off"
    $r = Invoke-WebRequest $url -WebSession $s -UseBasicParsing
    Write-Host $r.Content
}

Write-Host "--- TEST MATEMATICI (Gia Esistenti) ---"
Test-Anima "quanto fa 15 + 27"
Test-Anima "100 diviso 4"

Write-Host "`n--- TEST DJ LINGUAGGIO NATURALE ---"
Test-Anima "mixa adesso" "it"
Test-Anima "skip and mix" "en"
Test-Anima "attiva la modalita dj" "it"
Test-Anima "turn off the auto dj" "en"
Test-Anima "metti su hard" "it"
Test-Anima "musica piu tranquilla" "it"
Test-Anima "increase the energy" "en"

Write-Host "`n--- TEST ADVERSARIALI ANTI-FALSE-POSITIVI ---"
Test-Anima "cos'è un dj?" "it"          # Deve fallire o essere FACT, NON attivare il DJ
Test-Anima "come si mixa la musica?" "it" # Deve fallire o essere FACT
Test-Anima "spegni la luce" "it"          # Deve essere set_brightness
Test-Anima "alza il volume" "it"          # Deve essere set_volume
Test-Anima "ho bisogno di energia" "it"   # Manca il contesto musicale, non deve fare Vibe Up
Test-Anima "how do you play music?" "en"  # Deve fallire o essere FACT
