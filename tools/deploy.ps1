# Smart incremental deploy of the NucleoOS SD content.
#
# Stages the repo -> deploy/sd copying ONLY files whose content actually changed
# (SHA-256), then optionally mirrors deploy/sd -> a target SD drive the same way.
# Professional & safe: manifest-driven skip, atomic per-file writes (.nctmp + rename),
# post-copy hash verification, and mirror-delete of stale files. Nothing big is recopied
# unless it changed by even a single byte.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\deploy.ps1                 # stage only
#   powershell -ExecutionPolicy Bypass -File tools\deploy.ps1 -To H:\         # stage + push to SD
#   powershell -ExecutionPolicy Bypass -File tools\deploy.ps1 -To H:\ -DryRun # preview, no writes
param([string]$To, [switch]$DryRun)
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

# Device-state files: provisioned by the USER at runtime (API keys, learned cards, settings,
# telemetry). The release system must NEVER stage, overwrite, or mirror-delete these — a key on
# the SD always wins over anything in the repo. Mirrors the protection in sd-sync.ps1 / sd_deploy.py.
# Match is against the forward-slash SD-relative path.
$STATE = @(
    'data/anima/teacher.json',          # online provider config + API key
    # learned/ is NOT excluded wholesale: facets.<lang>.jsonl are firmware-hash-pinned READ-ONLY seeds and
    # MUST ship (byte-match VKL_FACETS_* in the .bin). Protect only the DEVICE-WRITTEN files here by name:
    'data/anima/learned/it.jsonl', 'data/anima/learned/en.jsonl',   # online answer cache
    'data/anima/learned/it.vec', 'data/anima/learned/en.vec',       # cache embeddings
    'data/anima/learned/mind.*.jsonl',                              # runtime KGE triples (mind_put)
    'data/anima/learned/knowledge.ledger.jsonl', 'data/anima/learned/evo/*',   # evolution ledger
    'data/anima/telemetry.ndjson', 'data/anima/session.txt', 'data/anima/sessions.json',
    'data/anima/workspace.json', 'data/anima/*.httptrace',
    'system/config/*', 'system/keys/*', 'system/sessions/*',   # runtime user settings + pairing key + sessions
    'system/log/*', 'system/logs/*',                            # device logs
    'config/*', 'backups/*', 'journal/*',                       # runtime config / backups / journal
    'auth.json', 'volume.json', 'settings.json', '*.vec'        # settings.json = root-level only (system/registry/settings.json is a system default)
)
function Is-State($rel) {
    foreach ($p in $STATE) { if ($rel -like $p) { return $true } }
    # ANIMA subtree is ALLOWLISTED like the firmware (nucleo_fs_is_protected): the ONLY things deploy
    # ships under data/anima are the system knowledge (akb5 shards; anima-*/dict-*/commands* files),
    # the firmware-pinned facets seeds, and the create-only workspace default. EVERYTHING ELSE there
    # — teacher.json (API key), learned caches, profile, presets, sessions, *.vec — is user state and
    # is never staged, overwritten, nor mirror-deleted. New ANIMA state files are protected for free.
    if ($rel -like 'data/anima/*') {
        if ($rel -like 'data/anima/akb5/*') { return $false }                                          # shards: ship
        $b = Split-Path $rel -Leaf
        if ($b -like 'facets.*.jsonl') { return $false }                                               # seeds: ship
        if ($b -like 'anima-*' -or $b -like 'dict-*' -or $b -like 'commands*') { return $false }       # encoder/index/dict/commands: ship
        if ($rel -eq 'data/anima/workspace.json') { return $false }                                    # default: create-only
        return $true                                                                                   # else: user state
    }
    return $false
}

function Load-Manifest($root) {
    $p = Join-Path $root $MANIFEST; $h = @{}
    if (Test-Path $p) { (Get-Content $p -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $h[$_.Name] = $_.Value } }
    return $h
}
function Save-Manifest($root, $man) {
    if ($DryRun) { return }
    if (-not (Test-Path $root)) { New-Item -ItemType Directory -Force -Path $root | Out-Null }
    ($man | ConvertTo-Json -Depth 4) | Out-File (Join-Path $root $MANIFEST) -Encoding utf8
}
function FileHash($path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash }

function Copy-IfChanged($src, $dst, $key, $man, $seen, $stat) {
    $seen[$key] = $true
    $fi = Get-Item -LiteralPath $src
    $size = $fi.Length.ToString(); $mtime = $fi.LastWriteTimeUtc.Ticks.ToString()
    $m = $man[$key]
    if ($m -and "$($m.size)" -eq $size -and "$($m.mtime)" -eq $mtime) { $stat.skipped++; return }  # unchanged (fast path)
    $hash = FileHash $src
    if ($m -and "$($m.hash)" -eq $hash) {                                                            # touched but identical -> no copy
        $man[$key] = [pscustomobject]@{ size = $size; mtime = $mtime; hash = $hash }; $stat.skipped++; return
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

function Sync-Dir($srcRoot, $dstRoot, $prefix, $man, $seen, $stat) {
    if (-not (Test-Path $srcRoot)) { return }
    Get-ChildItem -LiteralPath $srcRoot -Recurse -File | ForEach-Object {
        $rel = ($_.FullName.Substring($srcRoot.Length).TrimStart('\', '/')) -replace '\\', '/'
        if ($rel -eq $MANIFEST) { return }
        $key = if ($prefix) { "$prefix/$rel" } else { $rel }
        if (Is-State $key) { return }   # never stage/push a user's key or runtime state
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
Sync-Dir "$repo\tools\sd-sim\data" $sd 'data'                   $man $seen $stat
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
$exe = "$repo\windows-app\dist\NucleoConnect.exe"
if (Test-Path $exe) { Copy-IfChanged $exe (Join-Path $sd 'www\shell\downloads\NucleoConnect.exe') 'www/shell/downloads/NucleoConnect.exe' $man $seen $stat }
else { Write-Warning "NucleoConnect.exe missing - run: dotnet publish (windows-app)" }

# NucleoMind Android companion (Ollama-for-Android LLM server). Stable source = web\downloads\NucleoMind.apk;
# refresh it from the Gradle build (nucleomind\app\build\outputs\apk\debug\app-debug.apk) when rebuilt.
$apk = "$repo\web\downloads\NucleoMind.apk"
if (Test-Path $apk) { Copy-IfChanged $apk (Join-Path $sd 'www\shell\downloads\NucleoMind.apk') 'www/shell/downloads/NucleoMind.apk' $man $seen $stat }
else { Write-Warning "NucleoMind.apk missing - build nucleomind in Android Studio, then copy app-debug.apk to web\downloads\NucleoMind.apk" }

Write-Host "Compressing Web App files (GZIP) to save network RAM..."
Get-ChildItem -Path $sd -Recurse -Include *.js,*.css,*.html | ForEach-Object {
    $out = "$($_.FullName).gz"
    if (-not ((Test-Path $out) -and (Get-Item $out).LastWriteTimeUtc -ge $_.LastWriteTimeUtc)) {
        $inStream = [System.IO.File]::OpenRead($_.FullName)
        $outStream = [System.IO.File]::Create($out)
        $gzip = New-Object System.IO.Compression.GZipStream($outStream, [System.IO.Compression.CompressionMode]::Compress)
        $inStream.CopyTo($gzip)
        $gzip.Dispose(); $outStream.Dispose(); $inStream.Dispose()
    }
    $rel = ($out.Substring($sd.Length).TrimStart('\', '/')) -replace '\\', '/'
    $seen[$rel] = $true
}

Apply-Mirror $sd $seen $man $stat
Save-Manifest $sd $man
Report "Stage (deploy/sd)" $stat

# 2) Optional: mirror deploy/sd -> target SD drive (incremental)
if ($To) {
    if (-not (Test-Path $To)) { throw "target not found: $To" }
    # SAFETY: only ever write to a removable, non-system, non-boot drive.
    $dl = $To.TrimEnd('\', ':').Substring(0, 1)
    $vol = Get-Volume -DriveLetter $dl -ErrorAction Stop
    $tdisk = Get-Disk -Number (Get-Partition -DriveLetter $dl).DiskNumber
    if ($vol.DriveType -ne 'Removable' -or $tdisk.IsSystem -or $tdisk.IsBoot) {
        throw "SAFETY ABORT: $To ($($tdisk.FriendlyName), $($vol.DriveType)) is not a removable non-system drive"
    }
    Write-Host "Target OK: $dl`: $($tdisk.FriendlyName) ($($vol.DriveType), $([math]::Round($vol.Size/1GB,1)) GB)"
    $tman = Load-Manifest $To; $tseen = @{}; $tstat = @{ copied = 0; skipped = 0; deleted = 0; bytes = 0 }
    Sync-Dir $sd $To '' $tman $tseen $tstat
    Apply-Mirror $To $tseen $tman $tstat
    Save-Manifest $To $tman
    Report "Push ($To)" $tstat

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
        Write-Host "Voce TTS: data/tts -> $To (da deploy/sd-safe, mai cancellata)"
    } else {
        Write-Warning "Voce TTS assente in deploy/sd-safe/data/tts - ricostruisci con: node oversized-assets/rejoin.mjs tts-it-clips tts-en-clips"
    }
}
