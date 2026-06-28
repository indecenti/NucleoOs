# NucleoOS version bump — the single writer of firmware/version/{VERSION,BUILD}.
#
#   tools\version-bump.ps1                  # default: BUILD++  (every scripted build calls this)
#   tools\version-bump.ps1 -Bump patch      # 0.2.4 -> 0.2.5, BUILD reset to 0  (real release)
#   tools\version-bump.ps1 -Bump minor      # 0.2.4 -> 0.3.0, BUILD reset to 0
#   tools\version-bump.ps1 -Bump major      # 0.2.4 -> 1.0.0, BUILD reset to 0
#   tools\version-bump.ps1 -Set 1.0.0        # set semver explicitly, BUILD reset to 0
#
# The composed version string that ships (e.g. 0.2.5+0.g1a2b3c4) is assembled at BUILD time by
# firmware/version/version.cmake (it appends the build counter + git short hash + dirty flag) and
# baked into the ESP-IDF app descriptor. This script only moves the two on-disk source numbers.
#
# Bumping VERSION (a release) is a real source change you commit. Bumping BUILD happens on every
# build and is excluded from the firmware "dirty" check, so the counter churn alone never flags the
# tree dirty — but DO commit the BUILD bump alongside a release so the number is monotonic in history.
param(
    [ValidateSet('build','patch','minor','major')] [string]$Bump = 'build',
    [string]$Set = ''
)
$ErrorActionPreference = 'Stop'
$dir       = Join-Path (Split-Path $PSScriptRoot -Parent) 'firmware\version'
$verFile   = Join-Path $dir 'VERSION'
$buildFile = Join-Path $dir 'BUILD'

$semver = (Get-Content $verFile -Raw).Trim()
$build  = [int]((Get-Content $buildFile -Raw).Trim())

if ($Set) {
    if ($Set -notmatch '^\d+\.\d+\.\d+$') { Write-Error "-Set expects a semver like 1.2.3 (got '$Set')"; exit 1 }
    $semver = $Set
    $build  = 0
} elseif ($Bump -eq 'build') {
    $build++
} else {
    if ($semver -notmatch '^\d+\.\d+\.\d+$') { Write-Error "VERSION file is not semver: '$semver'"; exit 1 }
    $p = $semver.Split('.'); [int]$ma = $p[0]; [int]$mi = $p[1]; [int]$pa = $p[2]
    switch ($Bump) {
        'major' { $ma++; $mi = 0; $pa = 0 }
        'minor' { $mi++; $pa = 0 }
        'patch' { $pa++ }
    }
    $semver = "$ma.$mi.$pa"
    $build  = 0
}

# Write WITHOUT a trailing newline + ASCII so CMake file(STRINGS) and Get-Content both read one clean line.
[System.IO.File]::WriteAllText($verFile,   $semver, [System.Text.Encoding]::ASCII)
[System.IO.File]::WriteAllText($buildFile, "$build", [System.Text.Encoding]::ASCII)

Write-Host ("version -> {0}  build {1}   (ships as {0}+{1}.g<git>[*])" -f $semver, $build) -ForegroundColor Green
