$src = "g:\Nucleo\web\shell\shell.js"
$dest = "g:\Nucleo\deploy\sd\www\shell\shell.js"
$destGz = "g:\Nucleo\deploy\sd\www\shell\shell.js.gz"

Copy-Item $src -Destination $dest -Force
$bytes = [System.IO.File]::ReadAllBytes($dest)
$ms = New-Object System.IO.MemoryStream
$gz = New-Object System.IO.Compression.GZipStream($ms, [System.IO.Compression.CompressionMode]::Compress)
$gz.Write($bytes, 0, $bytes.Length)
$gz.Close()
[System.IO.File]::WriteAllBytes($destGz, $ms.ToArray())
Write-Host "Shell.js compressed successfully."
