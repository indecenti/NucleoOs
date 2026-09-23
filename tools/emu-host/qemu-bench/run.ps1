# qemu-bench — what one emulated Game Boy frame costs the ESP32-S3 CPU, in instructions.
#
# Builds a minimal ESP-IDF app that runs the vendored Game Boy core over six real cartridges and runs
# it in Espressif's QEMU with -icount shift=0 (one instruction = one virtual ns), so esp_timer's
# microseconds read directly as THOUSANDS OF INSTRUCTIONS per frame. Deterministic, host-only.
#
# What it cannot see: the flash instruction-cache misses that dominate on the real board (the device
# measures ~4x more time than the instruction count explains) — pair it with the on-card trace.
#
#   powershell -File tools\emu-host\qemu-bench\run.ps1                       # shipped config
#   powershell -File tools\emu-host\qemu-bench\run.ps1 -Core old -Jt 0 -Hilcd 0 -OldRev <commit>
param([string]$Core = "new", [string]$Hilcd = "1", [string]$Jt = "1", [string]$Opt = "-O2",
      [string]$OldRev = "HEAD", [string]$Tag = "run")
$ErrorActionPreference = 'Continue'
$B = $PSScriptRoot
$Repo = Resolve-Path (Join-Path $B "..\..\..")
$env:IDF_TOOLS_PATH = "$env:USERPROFILE\.espressif"
. C:\esp\esp-idf\export.ps1 *> $null

# ROMs are not committed: take them from the SD simulator's library.
$lib = Join-Path $Repo "tools\sd-sim\data\ROMs\gb"
$pick = @{ "tetris.gb" = "Tetris (World) (Rev A).gb"; "kirby.gb" = "Kirby's Dream Land (USA, Europe).gb";
           "zelda.gb" = "Legend of Zelda, The - Link's Awakening (USA, Europe) (Rev B).gb";
           "sf2.gb" = "Street Fighter II (USA, Europe) (Rev A).gb"; "tetris2.gb" = "Tetris 2 (USA).gb";
           "mm5.gb" = "Megaman V (USA).gb" }
New-Item -ItemType Directory -Force "$B\roms" | Out-Null
foreach ($k in $pick.Keys) {
  $src = Join-Path $lib $pick[$k]
  if (-not (Test-Path -LiteralPath $src)) { Write-Output "missing ROM: $src"; exit 1 }
  Copy-Item -LiteralPath $src "$B\roms\$k" -Force
}
if ($Core -eq "old") {
  git -C $Repo show "${OldRev}:firmware/components/nucleo_emu/vendor/peanut_gb.h" | Out-File -Encoding ascii "$B\main\peanut_old.h"
}

Set-Location $B
$env:BENCH_CORE = $Core; $env:BENCH_HILCD = $Hilcd; $env:BENCH_JT = $Jt; $env:BENCH_OPT = $Opt
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
& $nm -S --size-sort -t d "$B\build\gbbench.elf" | Select-String "__gb_step_cpu|__gb_draw_line|__gb_write|__gb_read"
