# sd-policy.ps1 - PowerShell reader of tools/lib/sd-policy.json (dot-source it). Same pattern semantics as
# tools/lib/sd-policy.mjs and sd_deploy.py: '**/' = zero or more segments, '**' = anything, '*' = within one
# segment, case-insensitive. Held to tools/lib/sd-policy-vectors.json by tools/sd-policy.test.mjs.
$script:SdPolicy = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'sd-policy.json') | ConvertFrom-Json

function ConvertTo-SdPolicyRegex([string]$glob) {
    $sb = New-Object System.Text.StringBuilder
    $i = 0
    while ($i -lt $glob.Length) {
        if ($glob.Substring($i).StartsWith('**/')) { [void]$sb.Append('(?:.*/)?'); $i += 3 }
        elseif ($glob.Substring($i).StartsWith('**')) { [void]$sb.Append('.*'); $i += 2 }
        elseif ($glob[$i] -eq '*') { [void]$sb.Append('[^/]*'); $i += 1 }
        else { [void]$sb.Append([regex]::Escape([string]$glob[$i])); $i += 1 }
    }
    return New-Object System.Text.RegularExpressions.Regex ('^' + $sb.ToString() + '$'), 'IgnoreCase'
}
$script:SdStateRe = @($script:SdPolicy.state | ForEach-Object { ConvertTo-SdPolicyRegex $_ })
$script:SdAnimaShipRe = @($script:SdPolicy.animaShip | ForEach-Object { ConvertTo-SdPolicyRegex $_ })

# Device / user state: never staged, shipped, pushed or overwritten by an SD tool.
function Is-DeviceState([string]$rel) {
    $p = ($rel -replace '\\', '/').TrimStart('/')
    foreach ($r in $script:SdStateRe) { if ($r.IsMatch($p)) { return $true } }
    if ($p -like 'data/anima/*') {
        foreach ($r in $script:SdAnimaShipRe) { if ($r.IsMatch($p)) { return $false } }
        return $true
    }
    return $false
}
