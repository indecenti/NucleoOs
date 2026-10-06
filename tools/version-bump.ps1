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
$oldSemver = (Get-Content $verFile -Raw).Trim()
[System.IO.File]::WriteAllText($verFile,   $semver, [System.Text.Encoding]::ASCII)
[System.IO.File]::WriteAllText($buildFile, "$build", [System.Text.Encoding]::ASCII)

# A release (the semver moved) carries the version everywhere it is declared, so the CI gate
# (tools/version-consistency.test.mjs) stays green: package.json, CITATION.cff, and a CHANGELOG.md
# section opened right under [Unreleased] (its bullets are still written by hand).
if ($semver -ne $oldSemver) {
    $root  = Split-Path $PSScriptRoot -Parent
    $today = Get-Date -Format 'yyyy-MM-dd'
    $utf8  = New-Object System.Text.UTF8Encoding($false)
    $pkg = Join-Path $root 'package.json'
    if (Test-Path $pkg) {
        $t = [System.IO.File]::ReadAllText($pkg)
        # The FIRST "version" only (the package's own). NB: [regex]::Replace's 4th argument is
        # RegexOptions, not a count — an instance Replace(input, replacement, count) limits it.
        $vre = New-Object System.Text.RegularExpressions.Regex('("version"\s*:\s*")[^"]*(")')
        $t = $vre.Replace($t, "`${1}$semver`${2}", 1)
        [System.IO.File]::WriteAllText($pkg, $t, $utf8)
    }
    $cff = Join-Path $root 'CITATION.cff'
    if (Test-Path $cff) {
        $t = [System.IO.File]::ReadAllText($cff)
        $t = [regex]::Replace($t, '(?m)^version:.*$', "version: $semver")
        $t = [regex]::Replace($t, '(?m)^date-released:.*$', "date-released: `"$today`"")
        [System.IO.File]::WriteAllText($cff, $t, $utf8)
    }
    $log = Join-Path $root 'CHANGELOG.md'
    if (Test-Path $log) {
        $t = [System.IO.File]::ReadAllText($log)
        if ($t -notmatch [regex]::Escape("## [$semver]")) {
            $nl = if ($t -match "`r`n") { "`r`n" } else { "`n" }
            $re = New-Object System.Text.RegularExpressions.Regex('## \[Unreleased\]')
            $dash = [char]0x2014   # an em dash, built here: Windows PowerShell 5.1 reads this file as ANSI
            $t = $re.Replace($t, "## [Unreleased]$nl$nl## [$semver] $dash $today", 1)
            [System.IO.File]::WriteAllText($log, $t, $utf8)
            Write-Host "CHANGELOG.md: the [Unreleased] notes are now under [$semver] - review them" -ForegroundColor Yellow
        }
    }
}

Write-Host ("version -> {0}  build {1}   (ships as {0}+{1}.g<git>[*])" -f $semver, $build) -ForegroundColor Green
