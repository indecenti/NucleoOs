# Smart incremental deploy of the NucleoOS SD content.
#
# Stages the repo -> deploy/sd copying ONLY files whose content actually changed
# (SHA-256), then optionally pushes deploy/sd -> a target SD drive the same way.
# Professional & safe: manifest-driven skip, atomic per-file writes (.nctmp + rename),
# post-copy hash verification. The STAGING tree (deploy/sd, owned by this script) is mirrored:
# files no longer in the sources are removed from it. The CARD (-To) is never mirrored: it also
# holds the user's own apps (Agent), downloaded models, device state and, on an M5Launcher card,
# other firmwares' files — the push only adds/updates, and system/registry/apps.json is MERGED
# (tools/lib/registry-merge.mjs), never overwritten. Stale files on a card are removed by hand.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\deploy.ps1                 # stage only
#   powershell -ExecutionPolicy Bypass -File tools\deploy.ps1 -To H:\         # stage + push to SD
#   powershell -ExecutionPolicy Bypass -File tools\deploy.ps1 -To H:\ -DryRun # preview, no writes
#   -TestTarget : TEST HARNESS ONLY — accept a non-removable -To, and only one inside %TEMP%.
param([string]$To, [switch]$DryRun, [switch]$TestTarget)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$sd = Join-Path $repo 'deploy\sd'
$MANIFEST = '.deploy-manifest.json'

# Files that must NEVER be mirror-deleted, even if they are absent from the staging
# source dirs. These are user-provisioned media that intentionally live on the SD
# (project image AND the physical Cardputer card) but are too big / personal to keep
# in the repo source tree. Match is against the forward-slash relative path.
#   -> The 5 Wallace & Gromit shorts are pinned here per explicit request: keep forever.
#   (Now largely subsumed by Is-UserContent below — kept for documentation.)
$KEEP = @(
    'data/Videos/Wallace e Gromit - *.nfv',
    'data/Videos/Wallace e Gromit - *.mp3'
)
function Is-Protected($rel) {
    foreach ($p in $KEEP) { if ($rel -like $p) { return $true } }
    return $false
}

# User content (Music, Videos, Pictures, Recordings, ROMs, DOS, Documents, Notes, captures, ...)
# lives DIRECTLY on the SD and is never kept in the repo source tree, so it is always "unseen" by
# the staging walk. The mirror must NEVER delete it — otherwise every `deploy.ps1 -To <SD>` wipes
# the user's media. Everything under data/ is treated as user-owned EXCEPT data/anima/, which is
# ANIMA system knowledge that deploy actually manages (so stale shards can still be pruned).
function Is-UserContent($rel) {
    return ($rel -like 'data/*') -and -not ($rel -like 'data/anima/*')
}

# The on-device VOICE is an integral system asset that must always ship and must NEVER be
# mirror-deleted by any deploy run. Two packages: the nucleo_tts clip banks under data/tts/
# (the voice that SPEAKS) and the Vosk dictation models under apps/anima/www/vosk/models/
# (the mic that LISTENS). Mirrors firmware nucleo_fs_is_protected (which pins data/tts/<lang>
# and the whole /apps tree) so the SD copy and the on-device copy are protected the same way.
function Is-Voice($rel) {
    return ($rel -like 'data/tts/*') -or ($rel -like 'apps/anima/www/vosk/models/*')
}

# Device / user state (API keys, learned cards, settings, keys, sessions, the user's documents...) is NEVER
# staged, pushed, overwritten or mirror-deleted: a key on the SD always wins over anything in the repo. The
# table is tools/lib/sd-policy.json, shared with sd-sync.ps1, push-ota, sd-net-sync and sd_deploy.py
# (tools/lib/sd-policy.ps1 reads it; tools/sd-policy.test.mjs holds every reader to the same vectors).
. (Join-Path $PSScriptRoot 'lib/sd-policy.ps1')
function Is-State($rel) { return (Is-DeviceState $rel) }

function Load-Manifest($root) {
    $p = Join-Path $root $MANIFEST; $h = @{}
    if (Test-Path $p) { (Get-Content $p -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $h[$_.Name] = $_.Value } }
    return $h
}
function Save-Manifest($root, $man) {
    if ($DryRun) { return }
    if (-not (Test-Path $root)) { New-Item -ItemType Directory -Force -Path $root | Out-Null }
    # Only entries whose file is really there (a key for a vanished file made push-ota read a file that does
    # not exist), sorted (stable diffs), UTF-8 WITHOUT a BOM and LF (Out-File -Encoding utf8 wrote a BOM that
    # made every JSON.parse of this manifest fail).
    $out = [ordered]@{}
    [string[]]$keys = @($man.Keys)
    [Array]::Sort($keys, [StringComparer]::Ordinal)                  # ordinal: identical on every machine/culture
    foreach ($k in $keys) {
        if (Test-Path -LiteralPath (Join-Path $root ($k -replace '/', '\')) -PathType Leaf) { $out[$k] = $man[$k] }
    }
    $json = ($out | ConvertTo-Json -Depth 4) -replace "`r`n", "`n"
    [IO.File]::WriteAllText((Join-Path $root $MANIFEST), $json + "`n", (New-Object Text.UTF8Encoding $false))
}
function FileHash($path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant() }   # lowercase, like sha256sum / sd_deploy.py

function Copy-IfChanged($src, $dst, $key, $man, $seen, $stat) {
    $seen[$key] = $true
    $fi = Get-Item -LiteralPath $src
    $size = $fi.Length.ToString(); $mtime = $fi.LastWriteTimeUtc.Ticks.ToString()
    $m = $man[$key]
    # The manifest only says what THIS script last wrote; the destination may have changed since (a file
    # removed on the device, a card written by another tool). Trust the fast paths only while the file is
    # actually there, and skip a copy whose bytes are already in place.
    $present = Test-Path -LiteralPath $dst
    $dstLen = if ($present) { (Get-Item -LiteralPath $dst).Length } else { -1 }
    # fast path: source unchanged since the last push AND the destination still has its size (a truncated or
    # rewritten file on the card is re-copied; a full byte check is what `sd_deploy.py verify` is for)
    if ($present -and $dstLen -eq $fi.Length -and $m -and "$($m.size)" -eq $size -and "$($m.mtime)" -eq $mtime) { $stat.skipped++; return }
    $hash = FileHash $src
    if ($present -and $dstLen -eq $fi.Length -and (($m -and "$($m.hash)" -eq $hash) -or (FileHash $dst) -eq $hash)) {
        $man[$key] = [pscustomobject]@{ size = $size; mtime = $mtime; hash = $hash }; $stat.skipped++; return   # identical bytes -> no copy
    }
    if (-not $DryRun) {
        $dir = Split-Path $dst -Parent; if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
        $tmp = "$dst.nctmp"
        Copy-Item -LiteralPath $src -Destination $tmp -Force
        if ((FileHash $tmp) -ne $hash) { Remove-Item $tmp -Force; throw "verify failed: $src" }      # post-copy integrity check
        Move-Item -LiteralPath $tmp -Destination $dst -Force                                          # atomic swap
    }
    $man[$key] = [pscustomobject]@{ size = $size; mtime = $mtime; hash = $hash }
    $stat.copied++; $stat.bytes += $fi.Length
}

function Sync-Dir($srcRoot, $dstRoot, $prefix, $man, $seen, $stat, $exclude = @()) {
    if (-not (Test-Path $srcRoot)) { return }
    Get-ChildItem -LiteralPath $srcRoot -Recurse -File | ForEach-Object {
        $rel = ($_.FullName.Substring($srcRoot.Length).TrimStart('\', '/')) -replace '\\', '/'
        if ($rel -eq $MANIFEST) { return }
        $key = if ($prefix) { "$prefix/$rel" } else { $rel }
        foreach ($x in $exclude) { if ($key -eq $x -or $key.StartsWith("$x/")) { return } }
        if (Is-State $key) { return }   # never stage/push a user's key or runtime state
        if ($script:CardPush -and $key -eq 'system/registry/apps.json') { return }   # merged separately on a card
        Copy-IfChanged $_.FullName (Join-Path $dstRoot ($key -replace '/', '\')) $key $man $seen $stat
    }
}

function Apply-Mirror($dstRoot, $seen, $man, $stat) {
    if (-not (Test-Path $dstRoot)) { return }
    Get-ChildItem -LiteralPath $dstRoot -Recurse -File | ForEach-Object {
        $rel = ($_.FullName.Substring($dstRoot.Length).TrimStart('\', '/')) -replace '\\', '/'
        if ($rel -eq $MANIFEST) { return }
        if (-not $seen.ContainsKey($rel)) {
            if (Is-Protected $rel)   { return }   # pinned media: never mirror-delete
            if (Is-Voice $rel)       { return }   # TTS clip banks / Vosk models: never mirror-delete
            if (Is-State $rel)       { return }   # user key / learned / settings: never mirror-delete
            if (Is-UserContent $rel) { return }   # user media/documents/captures: never mirror-delete
            if (-not $DryRun) { Remove-Item -LiteralPath $_.FullName -Force }
            $man.Remove($rel); $stat.deleted++
        }
    }
    if (-not $DryRun) {
        Get-ChildItem -LiteralPath $dstRoot -Recurse -Directory | Sort-Object { $_.FullName.Length } -Descending |
            Where-Object { -not (Get-ChildItem -LiteralPath $_.FullName -Force) } | Remove-Item -Force
    }
}

function Report($label, $stat) {
    "{0}: {1} copied ({2:N0} KB), {3} unchanged, {4} removed" -f $label, $stat.copied, ($stat.bytes / 1KB), $stat.skipped, $stat.deleted
}

# 0) Codegen: mirror the firmware's Costellazioni content tables into the web game (single source of
#    truth). Runs BEFORE staging so the generated apps/games/www/games/constellations-content.js ships.
$gen = Join-Path $PSScriptRoot 'gen-constellations-content.mjs'
if (Test-Path $gen) {
    Write-Host "Codegen: constellations content ->" -NoNewline
    & node $gen
    if ($LASTEXITCODE -ne 0) { throw "gen-constellations-content.mjs failed ($LASTEXITCODE)" }
}

# 0b) Codegen: pack the IR preset catalog (apps/ir-remote/www/presets.json) into the fixed-width
#     binary the firmware reads with O(1)-RAM fseek/fread (/sd/system/ir/presets.bin). Runs BEFORE
#     staging so the freshly-built pack ships; the staged source is tools\sd-sim\system\ir.
$irpack = Join-Path $PSScriptRoot 'ir-pack.mjs'
if (Test-Path $irpack) {
    Write-Host "Codegen: IR preset pack ->" -NoNewline
    & node $irpack "$repo\apps\ir-remote\www\presets.json" "$repo\tools\sd-sim\system\ir\presets.bin"
    if ($LASTEXITCODE -ne 0) { throw "ir-pack.mjs failed ($LASTEXITCODE)" }
}

# 1) Assemble repo -> deploy/sd (incremental)
$man = Load-Manifest $sd; $seen = @{}; $stat = @{ copied = 0; skipped = 0; deleted = 0; bytes = 0 }
Sync-Dir "$repo\registry"          $sd 'system/registry'        $man $seen $stat
Sync-Dir "$repo\apps"              $sd 'apps'                   $man $seen $stat
Sync-Dir "$repo\web\shell"         $sd 'www/shell'              $man $seen $stat
# The ANIMA knowledge (the shipped AKB5 manifest + exactly the shards it routes to) comes ONLY from
# deploy/sd-safe below — the same source sd_deploy.py release ships. The sd-sim tree carries its own 62-shard
# manifest and extra shards for the simulator; staging both made person.bin and the manifest ping-pong
# between the two copies on every run and shipped 15 unreferenced shards (~72 MB).
Sync-Dir "$repo\tools\sd-sim\data" $sd 'data'                   $man $seen $stat @('data/anima/akb5', 'data/anima/anima-it-akb5.bin')
Sync-Dir "$repo\tools\sd-sim\system\ir" $sd 'system/ir'         $man $seen $stat   # IR preset pack (presets.bin)
# Staging static assets from deploy/sd-safe
Sync-Dir "$repo\deploy\sd-safe\data\anima\akb5" $sd 'data/anima/akb5' $man $seen $stat
if (Test-Path "$repo\deploy\sd-safe\data\anima\anima-it-akb5.bin") {
    Copy-IfChanged "$repo\deploy\sd-safe\data\anima\anima-it-akb5.bin" (Join-Path $sd 'data\anima\anima-it-akb5.bin') 'data/anima/anima-it-akb5.bin' $man $seen $stat
}
Sync-Dir "$repo\deploy\sd-safe\evilportal"      $sd 'evilportal'     $man $seen $stat
Sync-Dir "$repo\deploy\sd-safe\wallpapers"      $sd 'wallpapers'     $man $seen $stat
if (Test-Path "$repo\deploy\sd-safe\README.md") {
    Copy-IfChanged "$repo\deploy\sd-safe\README.md" (Join-Path $sd 'README.md') 'README.md' $man $seen $stat
}
# NucleoConnect (Windows companion): a fresh local build (windows-app\dist, gitignored) wins; otherwise the
# committed copy in web\downloads — the same source sd_deploy.py ships. Without the fallback a machine that
# never built the Windows app mirror-deleted the tracked staged copy.
$exe = "$repo\windows-app\dist\NucleoConnect.exe"
if (-not (Test-Path $exe)) { $exe = "$repo\web\downloads\NucleoConnect.exe" }
if (Test-Path $exe) { Copy-IfChanged $exe (Join-Path $sd 'www\shell\downloads\NucleoConnect.exe') 'www/shell/downloads/NucleoConnect.exe' $man $seen $stat }
else { Write-Warning "NucleoConnect.exe missing - run: dotnet publish (windows-app)" }

# NucleoMind Android companion (Ollama-for-Android LLM server). Stable source = web\downloads\NucleoMind.apk;
# refresh it from the Gradle build (nucleomind\app\build\outputs\apk\debug\app-debug.apk) when rebuilt.
$apk = "$repo\web\downloads\NucleoMind.apk"
if (Test-Path $apk) { Copy-IfChanged $apk (Join-Path $sd 'www\shell\downloads\NucleoMind.apk') 'www/shell/downloads/NucleoMind.apk' $man $seen $stat }
else { Write-Warning "NucleoMind.apk missing - build nucleomind in Android Studio, then copy app-debug.apk to web\downloads\NucleoMind.apk" }

# Native-game SFX packs (data/<game>/pack/*.wav) live IN the staging tree: baked/copied there by
# tools/sfx-gen (npm run sfx:bake), there is no other source to stage them from. The games never synthesize
# on the device, so these WAVs are their only sound. Register them (in place: src = dst, never copied) so
# the manifest-driven Wi-Fi sync (push-ota --sync) ships them too, not only a card copy (sd-sync.ps1).
Get-ChildItem -LiteralPath (Join-Path $sd 'data') -Directory -ErrorAction SilentlyContinue | ForEach-Object {
    $pk = Join-Path $_.FullName 'pack'
    if (Test-Path -LiteralPath $pk) { Sync-Dir $pk $sd "data/$($_.Name)/pack" $man $seen $stat }
}

Write-Host "Compressing Web App files (GZIP) to save network RAM..."
Get-ChildItem -Path $sd -Recurse -Include *.js,*.css,*.html | ForEach-Object {
    $out = "$($_.FullName).gz"
    $rel = ($out.Substring($sd.Length).TrimStart('\', '/')) -replace '\\', '/'
    # A twin staged from the SOURCES (committed, kept fresh by check-gz) is authoritative: never regenerate
    # over it — a re-gzip here has different bytes, so the next run would re-stage the committed one (churn).
    if ($seen.ContainsKey($rel)) { return }
    if (-not ((Test-Path $out) -and (Get-Item $out).LastWriteTimeUtc -ge $_.LastWriteTimeUtc)) {
        if ($DryRun) { $seen[$rel] = $true; return }
        $inStream = [System.IO.File]::OpenRead($_.FullName)
        $outStream = [System.IO.File]::Create($out)
        $gzip = New-Object System.IO.Compression.GZipStream($outStream, [System.IO.Compression.CompressionMode]::Compress)
        $inStream.CopyTo($gzip)
        $gzip.Dispose(); $outStream.Dispose(); $inStream.Dispose()
    }
    $seen[$rel] = $true
    # record the generated twin like any staged file, so manifest consumers (push-ota) see it too
    $go = Get-Item -LiteralPath $out
    $man[$rel] = [pscustomobject]@{ size = $go.Length.ToString(); mtime = $go.LastWriteTimeUtc.Ticks.ToString(); hash = (FileHash $out) }
}

Apply-Mirror $sd $seen $man $stat
Save-Manifest $sd $man
Report "Stage (deploy/sd)" $stat

# 2) Optional: mirror deploy/sd -> target SD drive (incremental)
if ($To) {
    if (-not (Test-Path $To)) { throw "target not found: $To" }
    if ($TestTarget) {
        # Test harness seam: a fake card folder, and ONLY under the temp dir — never a real disk.
        $full = (Resolve-Path -LiteralPath $To).Path.TrimEnd('\')
        $tmp = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\')
        if (-not $full.StartsWith($tmp + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "SAFETY ABORT: -TestTarget only accepts a folder under $tmp (got $full)"
        }
        $To = $full
        Write-Host "Target OK: TEST folder $To"
    } else {
    # SAFETY: only ever write to a removable, non-system, non-boot drive.
    $dl = $To.TrimEnd('\', ':').Substring(0, 1)
    $vol = Get-Volume -DriveLetter $dl -ErrorAction Stop
    $tdisk = Get-Disk -Number (Get-Partition -DriveLetter $dl).DiskNumber
    if ($vol.DriveType -ne 'Removable' -or $tdisk.IsSystem -or $tdisk.IsBoot) {
        throw "SAFETY ABORT: $To ($($tdisk.FriendlyName), $($vol.DriveType)) is not a removable non-system drive"
    }
    Write-Host "Target OK: $dl`: $($tdisk.FriendlyName) ($($vol.DriveType), $([math]::Round($vol.Size/1GB,1)) GB)"
    }
    $tman = Load-Manifest $To; $tseen = @{}; $tstat = @{ copied = 0; skipped = 0; deleted = 0; bytes = 0 }
    # Add/update only — NO mirror on a card (see the header): nothing the card holds is ever deleted.
    $script:CardPush = $true
    Sync-Dir $sd $To '' $tman $tseen $tstat
    $script:CardPush = $false
    # Stale .gz twins — the one kind of file removed from a card. The device serves "<file>.gz" instead of
    # "<file>" in /www/shell and /apps/<id>/www, so a twin the staging does not ship, next to a raw file it
    # does, would shadow the new file. Same rule as the firmware (nucleo_fsapi fstwin.c) and
    # tools/lib/twin-scope.mjs; the regex is held to tools/lib/twin-scope-vectors.json by fstwin-check.mjs.
    $twins = 0
    foreach ($k in @($tseen.Keys)) {
        if ($k -match '^(www/shell|apps/[^/]+/www)/.' -and $k -notmatch '\.gz$' -and -not $tseen.ContainsKey("$k.gz")) {
            $g = Join-Path $To ("$k.gz" -replace '/', '\')
            if (Test-Path -LiteralPath $g -PathType Leaf) {
                if (-not $DryRun) { Remove-Item -LiteralPath $g -Force }
                $tman.Remove("$k.gz"); $twins++
            }
        }
    }
    if ($twins) { Write-Host "Stale .gz twins removed on the card: $twins" }
    Save-Manifest $To $tman
    Report "Push ($To)" $tstat

    # Registry: MERGE the staged apps.json into the card's copy — the release is authoritative for bundled
    # apps, the user's Agent-published apps (created_by "agent") are kept (tools/lib/registry-merge.mjs).
    $regSrc = Join-Path $sd 'system\registry\apps.json'
    $regDst = Join-Path $To 'system\registry\apps.json'
    if (Test-Path $regSrc) {
        $out = if ($DryRun) { [IO.Path]::GetTempFileName() } else { $regDst }
        if (-not $DryRun) { New-Item -ItemType Directory -Force -Path (Split-Path $regDst) | Out-Null }
        $dev = if (Test-Path $regDst) { $regDst } else { '-' }
        $res = & node (Join-Path $PSScriptRoot 'lib\registry-merge.mjs') $regSrc $dev $out
        if ($LASTEXITCODE -ne 0) { throw "registry merge failed (exit $LASTEXITCODE)" }
        if ($DryRun) { Remove-Item $out -ErrorAction SilentlyContinue }
        Write-Host "Registry ($To): $res"
    }

    # Voice that SPEAKS: the nucleo_tts clip banks (data/tts/, ~800 MB) are too big for the
    # git-tracked deploy/sd staging, so they ship straight from deploy/sd-safe. No /MIR -> only
    # adds/updates, never deletes; Is-Voice also shields them from the mirror pass above. The
    # Vosk models (the mic that LISTENS) ride the normal apps/ flow and are already pushed.
    $ttsSrc = Join-Path $repo 'deploy\sd-safe\data\tts'
    if (Test-Path (Join-Path $ttsSrc 'it\clips.pcm')) {
        if (-not $DryRun) {
            & robocopy $ttsSrc (Join-Path $To 'data\tts') *.* /E /NJH /NJS /NDL /NFL /NP /R:1 /W:1 | Out-Null
            if ($LASTEXITCODE -ge 8) { throw "robocopy TTS ha riportato un errore (exit $LASTEXITCODE)" }
        }
        Write-Host "TTS voice: data/tts -> $To (from deploy/sd-safe, never mirror-deleted)"
    } else {
        Write-Warning "TTS voice missing in deploy/sd-safe/data/tts - fetch it with: node oversized-assets/rejoin.mjs tts-it-clips tts-en-clips"
    }
}
