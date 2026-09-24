# qemu-bench-gg — what one emulated Game Gear frame costs the ESP32-S3 CPU, in instructions.
#
# Builds a minimal ESP-IDF app that runs the REAL nucleo_gg.c over six real cartridges and runs
# it in Espressif's QEMU with -icount shift=0 (one instruction = one virtual ns), so esp_timer's
# microseconds read directly as THOUSANDS OF INSTRUCTIONS per frame. Deterministic, host-only.
#
# What it cannot see: the flash instruction-cache misses that dominate on the real board (the device
# measures ~4x more time than the instruction count explains) — pair it with the on-card trace.
#
#   powershell -File tools\emu-host\qemu-bench-gg\run.ps1              # shipped config (-O2)
#   powershell -File tools\emu-host\qemu-bench-gg\run.ps1 -Opt -Os     # size-optimised variant
param([string]$Opt = "-O2", [string]$Skip = "0", [string]$Tag = "run")
$ErrorActionPreference = 'Continue'
$B = $PSScriptRoot
$Repo = Resolve-Path (Join-Path $B "..\..\..")
$env:IDF_TOOLS_PATH = "$env:USERPROFILE\.espressif"
. C:\esp\esp-idf\export.ps1 *> $null

# ROMs are not committed: take them from the SD simulator's library.
$lib = Join-Path $Repo "tools\sd-sim\data\ROMs\gg"
$pick = @{ "sonic.gg" = "Sonic the Hedgehog (World) (v1.1).gg"; "sonic2.gg" = "Sonic the Hedgehog 2 (World).gg";
           "chaos.gg" = "Sonic Chaos (USA, Europe).gg"; "mm2.gg" = "Micro Machines 2 - Turbo Tournament (Europe).gg";
           "ewj.gg" = "Earthworm Jim (USA, Europe).gg"; "sor2.gg" = "Streets of Rage II (World).gg" }
New-Item -ItemType Directory -Force (Join-Path $B "roms") | Out-Null
foreach ($k in $pick.Keys) {
  $src = Join-Path $lib $pick[$k]
  if (-not (Test-Path -LiteralPath $src)) { Write-Output "missing ROM: $src"; exit 1 }
  Copy-Item -LiteralPath $src (Join-Path (Join-Path $B "roms") $k) -Force
}

Set-Location $B
$env:BENCH_OPT = $Opt; $env:BENCH_SKIP = $Skip
if (-not (Test-Path "$B\build\CMakeCache.txt")) { idf.py set-target esp32s3 *> $null }
idf.py reconfigure *> $null                      # the variant knobs are read at configure time
idf.py build *> "$B\build_$Tag.log"
if ($LASTEXITCODE -ne 0) { Write-Output "BUILD FAILED"; Get-Content "$B\build_$Tag.log" -Tail 30; exit 1 }
Set-Location "$B\build"
python -m esptool --chip esp32s3 merge_bin --fill-flash-size 8MB -o "$B\build\flash_$Tag.bin" "@flash_args" *> $null
$q = (Get-ChildItem "$env:USERPROFILE\.espressif\tools\qemu-xtensa" -Recurse -Filter qemu-system-xtensa.exe | Select-Object -First 1).FullName
$out = "$B\build\qemu_$Tag.txt"
Remove-Item $out -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $q -ArgumentList @("-nographic", "-machine", "esp32s3",
      "-drive", "file=$B\build\flash_$Tag.bin,if=mtd,format=raw", "-icount", "shift=0",
      "-serial", "file:$out", "-monitor", "none") -PassThru -WindowStyle Hidden
$deadline = (Get-Date).AddMinutes(25)
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Seconds 5
  if ((Test-Path $out) -and (Select-String -Path $out -Pattern "@@END" -Quiet)) { break }
}
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Select-String -Path $out -Pattern "@@" | ForEach-Object { $_.Line }
$nm = (Get-ChildItem "$env:USERPROFILE\.espressif\tools\xtensa-esp-elf" -Recurse -Filter xtensa-esp32s3-elf-nm.exe | Select-Object -First 1).FullName
& $nm -S --size-sort -t d "$B\build\ggbench.elf" | Select-String "emulate|nucleo_gg_run_frame|gg_fault|psg_run"
