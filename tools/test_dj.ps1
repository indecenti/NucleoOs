$ErrorActionPreference = "Stop"

# Compile test harness
gcc -o test_dj.exe test_dj.c ../firmware/components/nucleo_audio/nucleo_dj_planner.c
if ($LASTEXITCODE -ne 0) {
    Write-Host "Compilation failed" -ForegroundColor Red
    exit $LASTEXITCODE
}

# Run test harness
.\test_dj.exe
